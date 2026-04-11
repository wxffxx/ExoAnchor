"""
Skill Store — Load, save, index, import/export skills.
"""

import importlib.util
import inspect
import os
import shutil
import logging
from typing import Optional

import yaml

from .skill import Skill, SkillBase, PythonSkillAdapter

logger = logging.getLogger("exoanchor.skills.store")


class SkillStore:
    """
    Manages the skill library — loading, saving, indexing, import/export.

    Skills are stored as YAML files organized in directories:
      - _builtin/: Built-in skills (bootstrap SSH, wait for OS, etc.)
      - bios/: BIOS-related skills
      - system/: System administration skills
      - benchmark/: Performance testing skills
      - custom/: User-created skills
    """

    def __init__(self, library_dir: str, builtin_dir: str = None):
        self.library_dir = os.path.abspath(library_dir)
        self.builtin_dir = builtin_dir or os.path.join(
            os.path.dirname(__file__), "_builtin"
        )
        self._skills: dict[str, Skill] = {}
        self._index_loaded = False

    def load_all(self):
        """Scan and load all skills from the library and builtin directories."""
        self._skills.clear()

        # Load builtin skills
        if os.path.isdir(self.builtin_dir):
            self._scan_directory(self.builtin_dir)

        # Load user library
        if os.path.isdir(self.library_dir):
            self._scan_directory(self.library_dir)

        self._index_loaded = True
        logger.info(f"Loaded {len(self._skills)} skills")

    def _scan_directory(self, directory: str):
        """Recursively scan a directory for skill YAML files."""
        for root, dirs, files in os.walk(directory):
            for f in files:
                path = os.path.join(root, f)
                if f.endswith((".yaml", ".yml")) and not f.startswith("_index"):
                    try:
                        skill = Skill.from_yaml_file(path)
                        self._skills[skill.name] = skill
                        logger.debug(f"Loaded skill: {skill.name} from {path}")
                    except Exception as e:
                        logger.warning(f"Failed to load skill from {path}: {e}")
                elif f.endswith(".py") and not f.startswith("_"):
                    try:
                        for skill in self._load_python_skills(path):
                            self._skills[skill.name] = skill
                            logger.debug(f"Loaded python skill: {skill.name} from {path}")
                    except Exception as e:
                        logger.warning(f"Failed to load python skill from {path}: {e}")

    def _load_python_skills(self, path: str) -> list[PythonSkillAdapter]:
        """Load one or more Python SkillBase subclasses from a module."""
        module_name = f"exoanchor_dynamic_skill_{abs(hash(os.path.abspath(path)))}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"Could not create import spec for {path}")

        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        loaded = []
        builtin = os.path.commonpath([os.path.abspath(path), os.path.abspath(self.builtin_dir)]) == os.path.abspath(self.builtin_dir)
        for _, obj in inspect.getmembers(module, inspect.isclass):
            if not issubclass(obj, SkillBase) or obj is SkillBase:
                continue
            if obj.__module__ != module.__name__:
                continue
            loaded.append(PythonSkillAdapter(obj(), source_path=path, builtin=builtin))

        if not loaded:
            raise RuntimeError("No SkillBase subclass found")
        return loaded

    def list_skills(self, tags: list[str] = None) -> list[dict]:
        """List all available skills, optionally filtered by tags."""
        if not self._index_loaded:
            self.load_all()

        results = []
        for skill in self._skills.values():
            if tags:
                if not any(t in skill.tags for t in tags):
                    continue
            results.append(skill.to_dict())

        return sorted(results, key=lambda s: s["name"])

    def get_skill(self, name: str) -> Optional[Skill]:
        """Get a skill by name."""
        if not self._index_loaded:
            self.load_all()
        return self._skills.get(name)

    def save_skill(self, name: str, content: str, category: str = "custom") -> str:
        """
        Save a skill YAML to the library.
        Returns the file path.
        """
        category_dir = os.path.join(self.library_dir, category)
        os.makedirs(category_dir, exist_ok=True)

        # Sanitize filename
        safe_name = name.replace("/", "_").replace("\\", "_")
        path = os.path.join(category_dir, f"{safe_name}.yaml")

        with open(path, "w") as f:
            f.write(content)

        # Reload
        try:
            skill = Skill.from_yaml_file(path)
            self._skills[skill.name] = skill
            logger.info(f"Saved skill '{skill.name}' to {path}")
        except Exception as e:
            logger.error(f"Saved file but failed to parse: {e}")

        return path

    def delete_skill(self, name: str) -> bool:
        """Delete a skill from the library."""
        skill = self._skills.get(name)
        if not skill:
            return False

        if skill.is_builtin:
            logger.warning(f"Cannot delete builtin skill: {name}")
            return False

        if skill.source_path and os.path.exists(skill.source_path):
            os.remove(skill.source_path)
            logger.info(f"Deleted skill file: {skill.source_path}")

        del self._skills[name]
        return True

    def import_skill(self, file_path: str, category: str = "custom") -> Optional[Skill]:
        """Import a skill from an external YAML file."""
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"Skill file not found: {file_path}")

        # Parse first to validate
        skill = Skill.from_yaml_file(file_path)

        # Copy to library
        category_dir = os.path.join(self.library_dir, category)
        os.makedirs(category_dir, exist_ok=True)
        dest = os.path.join(category_dir, os.path.basename(file_path))
        shutil.copy2(file_path, dest)

        # Reload from new location
        skill = Skill.from_yaml_file(dest)
        self._skills[skill.name] = skill
        logger.info(f"Imported skill '{skill.name}' from {file_path}")
        return skill

    def export_skill(self, name: str) -> Optional[str]:
        """Export a skill as YAML string."""
        skill = self._skills.get(name)
        if not skill:
            return None

        if skill.source_path and os.path.exists(skill.source_path):
            with open(skill.source_path, "r") as f:
                return f.read()

        return yaml.dump({"skill": skill._data}, default_flow_style=False, allow_unicode=True)
