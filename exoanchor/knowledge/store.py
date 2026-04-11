import os
import glob
import yaml
import logging

logger = logging.getLogger("exoanchor.knowledge")

class KnowledgeStore:
    """
    Lightweight RAG Knowledge Store.
    Parses all YAML/Markdown files in a specific directory and concatenates them 
    for prompt injection.
    """
    def __init__(self, directory: str = "exoanchor/knowledge"):
        self.directory = directory
        self.data = {}
        self._compiled_text = ""
        
    def load_all(self):
        """Load all knowledge files from the directory"""
        if not os.path.exists(self.directory):
            logger.info(f"Knowledge directory {self.directory} not found. Skipping.")
            return

        self.data.clear()
        
        # Load YAML files
        yaml_files = glob.glob(os.path.join(self.directory, "*.yml")) + glob.glob(os.path.join(self.directory, "*.yaml"))
        for fp in yaml_files:
            try:
                with open(fp, "r", encoding="utf-8") as f:
                    content = yaml.safe_load(f)
                    if content:
                        basename = os.path.basename(fp)
                        self.data[basename] = content
            except Exception as e:
                logger.error(f"Failed to load knowledge file {fp}: {e}")

        # Load Markdown files
        md_files = glob.glob(os.path.join(self.directory, "*.md"))
        for fp in md_files:
            try:
                with open(fp, "r", encoding="utf-8") as f:
                    basename = os.path.basename(fp)
                    self.data[basename] = f.read()
            except Exception as e:
                logger.error(f"Failed to load knowledge text {fp}: {e}")

        self._compile_text()
        logger.info(f"Loaded {len(self.data)} knowledge sources.")
        
    def _compile_text(self):
        """Compile loaded knowledge into a single text block for LLM prompt injection"""
        if not self.data:
            self._compiled_text = ""
            return
            
        blocks = []
        for src, content in self.data.items():
            blocks.append(f"--- Source: {src} ---")
            if isinstance(content, dict):
                blocks.append(yaml.dump(content, allow_unicode=True, default_flow_style=False))
            else:
                blocks.append(str(content))
                
        self._compiled_text = "\n".join(blocks)
        
    def get_prompt_injection(self) -> str:
        """Get the compiled text suitable for injection into SYSTEM_PROMPT"""
        return self._compiled_text
