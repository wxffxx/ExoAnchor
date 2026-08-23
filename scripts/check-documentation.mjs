#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";

const root = path.resolve(process.argv[2] || process.cwd());
const skippedDirs = new Set([
  ".git",
  "components",
  "managed_components",
  "node_modules",
  "__pycache__",
]);
const errors = [];
const markdownFiles = [];

function walk(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      if (skippedDirs.has(entry.name) || entry.name.startsWith("build")) {
        continue;
      }
      walk(path.join(directory, entry.name));
      continue;
    }
    if (entry.isFile() && entry.name.endsWith(".md")) {
      markdownFiles.push(path.join(directory, entry.name));
    }
  }
}

function relative(file) {
  return path.relative(root, file) || ".";
}

function localTarget(rawTarget) {
  let target = rawTarget.trim();
  if (target.startsWith("<") && target.endsWith(">")) {
    target = target.slice(1, -1);
  }
  target = target.split(/\s+(?=["'])/u, 1)[0];
  if (
    !target ||
    target.startsWith("#") ||
    /^(?:https?:|mailto:|data:)/iu.test(target)
  ) {
    return null;
  }
  target = target.split("#", 1)[0].split("?", 1)[0];
  try {
    return decodeURIComponent(target);
  } catch {
    return target;
  }
}

walk(root);

const linkPattern = /!?\[[^\]]*\]\(([^)\n]+)\)/gu;
for (const file of markdownFiles) {
  const text = fs.readFileSync(file, "utf8");

  if (/\/Users\/[^/\s]+\/|file:\/\//u.test(text)) {
    errors.push(`${relative(file)}: 包含本机绝对路径或 file:// 引用`);
  }

  for (const match of text.matchAll(linkPattern)) {
    const target = localTarget(match[1]);
    if (!target) {
      continue;
    }
    const resolved = path.resolve(path.dirname(file), target);
    if (!fs.existsSync(resolved)) {
      errors.push(`${relative(file)}: 失效链接 -> ${target}`);
    }
  }
}

const forbiddenFilePattern =
  /(^|\/)(TODO|NOTES|.*AUDIT|.*FINDINGS|.*GAP)(_[^/]*)?\.md$/iu;
for (const file of markdownFiles) {
  const name = relative(file);
  if (
    name.includes("/docs/development/") ||
    name.startsWith("docs/development/") ||
    forbiddenFilePattern.test(name)
  ) {
    errors.push(`${name}: 开发记录应移入 codexws/dev-notes/`);
  }
}

const cmake = path.join(
  root,
  "device/ESP32P4/firmware/v0.86.6-dev/CMakeLists.txt",
);
if (fs.existsSync(cmake)) {
  const cmakeText = fs.readFileSync(cmake, "utf8");
  const rawVersion = cmakeText.match(/PROJECT_VER\s+"([^"]+)"/u)?.[1];
  const versionBase = cmakeText.match(/set\(SI_VERSION_BASE\s+"([^"]+)"\)/u)?.[1];
  const version = rawVersion?.replace(
    "${SI_VERSION_BASE}",
    versionBase ?? "${SI_VERSION_BASE}",
  );
  if (!version) {
    errors.push("无法从开发固件 CMakeLists.txt 读取 PROJECT_VER");
  } else {
    for (const required of [
      "README_zh.md",
      "README.md",
      "device/ESP32P4/README.md",
      "device/ESP32P4/firmware/README.md",
      "device/ESP32P4/firmware/v0.86.6-dev/README.md",
    ]) {
      const file = path.join(root, required);
      if (!fs.existsSync(file) || !fs.readFileSync(file, "utf8").includes(version)) {
        errors.push(`${required}: 未同步当前开发版本 ${version}`);
      }
    }
  }
}

if (errors.length > 0) {
  console.error("documentation check failed:");
  for (const error of errors) {
    console.error(`- ${error}`);
  }
  process.exit(1);
}

console.log(`documentation check: ok (${markdownFiles.length} Markdown files)`);
