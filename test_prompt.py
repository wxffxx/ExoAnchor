import json
import asyncio
import aiohttp
from exoanchor.runtime import SYSTEM_PROMPT, parse_llm_response

async def main():
    api_key = '' # I don't have the user's API key.
    # Actually I can just test the parser itself without generating!
