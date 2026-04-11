"""
SSH Channel Manager — Remote shell access to Ubuntu target machines.

Provides:
  - SSH connection management (asyncssh or system ssh)
  - Command execution
  - File transfer (SFTP)
  - Connection health monitoring
"""

import asyncio
import errno
import logging
import os
import platform
import select
import shlex
import shutil
import signal
import time
from typing import Optional

logger = logging.getLogger("exoanchor.channels.ssh")


class SSHChannelManager:
    """
    Manages SSH connection to the target Ubuntu machine.

    Usage:
        ssh = SSHChannelManager({"ip": "192.168.1.100", "username": "ubuntu"})
        await ssh.connect()
        output = await ssh.run("uname -a")
        await ssh.close()
    """

    def __init__(self, config: dict):
        self.target_ip = config.get("ip", "")
        self.port = config.get("port", 22)
        self.username = config.get("username", "root")
        self.key_file = config.get("key_file")
        self.password = config.get("password")
        self.backend = config.get("backend", "auto")
        self.client = None
        self._connected = False
        self._active_backend = None
        self._connect_lock = asyncio.Lock()

    def _resolve_backend(self) -> str:
        """Pick the SSH backend for the current runtime."""
        if self.backend in ("asyncssh", "system"):
            return self.backend
        if platform.system() != "Windows" and shutil.which("ssh"):
            return "system"
        return "asyncssh"

    def _build_system_ssh_command(self, remote_command: str, timeout: int) -> list[str]:
        """Build a system ssh command line."""
        target = f"{self.username}@{self.target_ip}"
        cmd = [
            "ssh",
            "-p", str(self.port),
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", f"ConnectTimeout={max(1, int(timeout))}",
            "-o", "ServerAliveInterval=15",
            "-o", "ServerAliveCountMax=3",
            "-o", "BatchMode=no",
            target,
            remote_command,
        ]

        if self.key_file:
            cmd[1:1] = ["-i", self.key_file]
        elif self.password:
            # Force password/kbd-interactive auth in dev so we don't accidentally
            # pick up local agent keys which may differ from the working terminal flow.
            cmd[1:1] = [
                "-o", "PubkeyAuthentication=no",
                "-o", "PreferredAuthentications=password,keyboard-interactive",
                "-o", "NumberOfPasswordPrompts=1",
            ]

        return cmd

    async def _run_system_ssh(self, remote_command: str, timeout: int = 300) -> dict:
        """
        Execute a command via the system ssh client.
        Delegates blocking PTY work to a thread pool to avoid blocking the event loop.
        """
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            None, self._run_system_ssh_blocking, remote_command, timeout
        )

    def _run_system_ssh_blocking(self, remote_command: str, timeout: int = 300) -> dict:
        """
        Synchronous implementation: forks a PTY child for the system ssh client
        so password prompts and keyboard-interactive auth work like a real terminal.
        """
        cmd = self._build_system_ssh_command(remote_command, timeout)
        logger.debug(f"SSH cmd (sanitised): {' '.join(c for c in cmd if 'assword' not in c.lower())}")
        output = ""
        password_sent = False
        start = time.time()
        child_pid, master_fd = os.forkpty()

        if child_pid == 0:
            try:
                os.execvp(cmd[0], cmd)
            except Exception as exc:
                os.write(2, f"Failed to exec ssh: {exc}\n".encode("utf-8", errors="replace"))
            os._exit(127)

        try:
            while True:
                elapsed = time.time() - start
                if elapsed > timeout:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except OSError:
                        pass
                    raise TimeoutError(f"SSH command timed out after {timeout}s: {remote_command}")

                ready, _, _ = select.select([master_fd], [], [], 0.2)
                if ready:
                    try:
                        raw = os.read(master_fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            raw = b""
                        else:
                            raise
                    chunk = raw.decode("utf-8", errors="replace")
                    if chunk:
                        output += chunk
                        # Detect password prompt in accumulated output (handles split reads)
                        if (
                            self.password
                            and not password_sent
                            and ("assword:" in output or "assword: " in output)
                        ):
                            time.sleep(0.05)  # tiny delay for prompt to fully render
                            os.write(master_fd, (self.password + "\n").encode("utf-8"))
                            password_sent = True
                            logger.debug("SSH: password sent to PTY")

                pid, status = os.waitpid(child_pid, os.WNOHANG)
                if pid == child_pid:
                    # Drain remaining output
                    while True:
                        ready, _, _ = select.select([master_fd], [], [], 0.05)
                        if not ready:
                            break
                        try:
                            raw = os.read(master_fd, 4096)
                        except OSError as exc:
                            if exc.errno == errno.EIO:
                                break
                            raise
                        chunk = raw.decode("utf-8", errors="replace")
                        if not chunk:
                            break
                        output += chunk
                    ret = os.waitstatus_to_exitcode(status)

                    # Strip password prompt and echo from visible output
                    clean_output = self._strip_password_noise(output)
                    return {
                        "success": ret == 0,
                        "exit_status": ret,
                        "stdout": clean_output,
                        "stderr": "",
                        "output": clean_output.strip(),
                    }
        finally:
            try:
                os.close(master_fd)
            except OSError:
                pass
            try:
                os.waitpid(child_pid, 0)
            except ChildProcessError:
                pass

    def _strip_password_noise(self, text: str) -> str:
        """Remove password prompts and echo from PTY output."""
        import re
        # Remove lines like "wxffxx@host's password: " and blank line after
        text = re.sub(r".*assword:\s*\r?\n?", "", text)
        # Remove ANSI escape sequences sometimes injected by PTY
        text = re.sub(r"\x1b\[[0-9;]*[a-zA-Z]", "", text)
        return text

    async def connect(self, timeout: int = 10) -> bool:
        """
        Establish SSH connection to target machine.
        Returns True on success, False on failure.
        """
        if not self.target_ip:
            logger.warning("SSH: No target IP configured")
            return False

        async with self._connect_lock:
            backend = self._resolve_backend()
            self._active_backend = backend

            if backend == "system":
                try:
                    result = await self._run_system_ssh("true", timeout=timeout)
                    self._connected = result["success"]
                    if self._connected:
                        logger.info(f"SSH connected via system ssh to {self.username}@{self.target_ip}:{self.port}")
                    else:
                        logger.warning(f"SSH connection failed: {result['output'] or 'unknown error'}")
                    return self._connected
                except Exception as e:
                    logger.warning(f"SSH connection failed: {e}")
                    self._connected = False
                    return False

            try:
                import asyncssh

                connect_kwargs = {
                    "host": self.target_ip,
                    "port": self.port,
                    "username": self.username,
                    "known_hosts": None,  # Accept any host key (KVM scenario)
                    "connect_timeout": timeout,
                }

                if self.key_file:
                    connect_kwargs["client_keys"] = [self.key_file]
                if self.password:
                    connect_kwargs["password"] = self.password

                self.client = await asyncssh.connect(**connect_kwargs)
                self._connected = True
                logger.info(f"SSH connected to {self.username}@{self.target_ip}:{self.port}")
                return True

            except ImportError:
                logger.error("asyncssh not installed. Run: pip install asyncssh")
                return False
            except Exception as e:
                logger.warning(f"SSH connection failed: {e}")
                self.client = None
                self._connected = False
                return False

    async def run(self, command: str, timeout: int = 300) -> str:
        """
        Execute a command on the target machine via SSH.
        Returns stdout as string.
        Raises ConnectionError if not connected.
        """
        result = await self.run_with_status(command, timeout=timeout)
        return result["stdout"]

    async def run_with_status(self, command: str, timeout: int = 300) -> dict:
        """
        Execute a command and return stdout/stderr plus exit status.
        Raises ConnectionError if not connected.
        """
        if self._active_backend == "system":
            if not self.has_shell and not await self.ensure_connected():
                raise ConnectionError("SSH not connected")
            return await self._run_system_ssh(command, timeout=timeout)

        if not self.has_shell:
            raise ConnectionError("SSH not connected")

        try:
            result = await asyncio.wait_for(
                self.client.run(command),
                timeout=timeout,
            )
            stdout = result.stdout or ""
            stderr = result.stderr or ""
            return {
                "success": result.exit_status == 0,
                "exit_status": result.exit_status,
                "stdout": stdout,
                "stderr": stderr,
                "output": (stdout or stderr).strip(),
            }
        except asyncio.TimeoutError:
            raise TimeoutError(f"SSH command timed out after {timeout}s: {command}")
        except Exception as e:
            # Connection might have dropped
            logger.error(f"SSH command failed: {e}")
            self.client = None
            raise

    async def run_stream(self, command: str, timeout: int = 300):
        """
        Execute a command and yield output chunks as they arrive.
        Yields ('stdout', text) chunks followed by a final
        ('done', {'success': bool, 'exit_status': int|None}) event.
        """
        if self._active_backend == "system":
            result = await self.run_with_status(command, timeout=timeout)
            if result["stdout"]:
                yield ("stdout", result["stdout"])
            yield ("done", {
                "success": result["success"],
                "exit_status": result["exit_status"],
            })
            return

        if not self.has_shell:
            raise ConnectionError("SSH not connected")

        try:
            marker = "__CORTEX_EXIT_STATUS__:"
            wrapped = (
                "{ "
                + command
                + f"; }} 2>&1; printf '\\n{marker}%s\\n' $?"
            )
            process = await self.client.create_process(
                f"bash -lc {shlex.quote(wrapped)}"
            )
            deadline = asyncio.get_event_loop().time() + timeout
            buffer = ""
            marker_guard = len(marker) + 32
            final_sent = False

            try:
                while True:
                    remaining = deadline - asyncio.get_event_loop().time()
                    if remaining <= 0:
                        yield ("stdout", f"\n[Timed out after {timeout}s]")
                        yield ("done", {"success": False, "exit_status": None})
                        final_sent = True
                        break
                    try:
                        data = await asyncio.wait_for(
                            process.stdout.read(1024),
                            timeout=min(remaining, 5.0)
                        )
                        if not data:
                            break
                        buffer += data
                        if len(buffer) > marker_guard:
                            flush_upto = len(buffer) - marker_guard
                            if flush_upto > 0:
                                yield ("stdout", buffer[:flush_upto])
                                buffer = buffer[flush_upto:]
                    except asyncio.TimeoutError:
                        # Check if process is still running
                        if process.is_closing():
                            break
                        continue
            finally:
                exit_status = None
                success = False
                marker_idx = buffer.rfind(marker)
                if marker_idx != -1:
                    visible = buffer[:marker_idx]
                    if visible:
                        yield ("stdout", visible)
                    suffix = buffer[marker_idx + len(marker):].strip().splitlines()
                    if suffix:
                        try:
                            exit_status = int(suffix[0].strip())
                        except ValueError:
                            exit_status = None
                elif buffer:
                    yield ("stdout", buffer)

                process.close()
                try:
                    await asyncio.wait_for(process.wait_closed(), timeout=5)
                except Exception:
                    pass
                if exit_status is None:
                    exit_status = getattr(process, "exit_status", None)
                success = (exit_status == 0)
                if not final_sent:
                    yield ("done", {"success": success, "exit_status": exit_status})

        except Exception as e:
            logger.error(f"SSH stream failed: {e}")
            # Don't set self.client = None here — stream failure shouldn't kill the connection
            raise

    async def run_check(self, command: str, timeout: int = 30) -> tuple[bool, str]:
        """
        Execute a command and return (success, output).
        success = True if exit code is 0.
        Never raises — returns (False, error_msg) on failure.
        """
        try:
            if not self.has_shell and not await self.ensure_connected():
                return False, "SSH not connected"

            if self._active_backend == "system":
                result = await self._run_system_ssh(command, timeout=timeout)
                return result["success"], result["output"]

            result = await asyncio.wait_for(
                self.client.run(command),
                timeout=timeout,
            )
            success = result.exit_status == 0
            output = result.stdout or result.stderr or ""
            return success, output.strip()
        except Exception as e:
            return False, str(e)

    async def upload(self, local_path: str, remote_path: str) -> None:
        """Upload a file to the target machine via SFTP."""
        if self._active_backend == "system":
            raise NotImplementedError("Upload is not supported with the system ssh backend yet")
        if not self.has_shell:
            raise ConnectionError("SSH not connected")

        async with self.client.start_sftp_client() as sftp:
            await sftp.put(local_path, remote_path)
            logger.info(f"Uploaded {local_path} → {remote_path}")

    async def download(self, remote_path: str, local_path: str) -> None:
        """Download a file from the target machine via SFTP."""
        if self._active_backend == "system":
            raise NotImplementedError("Download is not supported with the system ssh backend yet")
        if not self.has_shell:
            raise ConnectionError("SSH not connected")

        async with self.client.start_sftp_client() as sftp:
            await sftp.get(remote_path, local_path)
            logger.info(f"Downloaded {remote_path} → {local_path}")

    @property
    def has_shell(self) -> bool:
        """Whether SSH connection is active."""
        if self._active_backend == "system":
            return self._connected
        if self.client is None:
            return False
        # asyncssh: is_closed() is a METHOD, not a property
        try:
            return not self.client.is_closed()
        except TypeError:
            # Fallback: some versions may use it as property
            return not self.client.is_closed

    async def ensure_connected(self) -> bool:
        """Connect if not already connected. Returns True if connected."""
        if self.has_shell:
            if self._active_backend == "system":
                return True
            # Quick health check
            try:
                await self.run("echo ok", timeout=5)
                return True
            except Exception:
                self.client = None
                self._connected = False

        return await self.connect()

    async def close(self) -> None:
        """Close SSH connection."""
        self._connected = False
        if self.client:
            self.client.close()
            await self.client.wait_closed()
            self.client = None
            logger.info("SSH connection closed")

    def get_status(self) -> dict:
        """Get connection status."""
        return {
            "connected": self.has_shell,
            "backend": self._active_backend or self._resolve_backend(),
            "target": f"{self.username}@{self.target_ip}:{self.port}" if self.target_ip else "not configured",
        }
