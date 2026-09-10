#!/usr/bin/env python3
"""
scripts/fetch_providers.py — HalCode9000 Provider & Model Aggregator Script

Scrapes/queries LLM provider APIs (OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek, Groq, xAI, Ollama)
and generates or updates standard JSON configuration files in providers/ and ~/.halcode/providers/.

Automatically loads API keys from ~/.halcode/keys.env and environment variables.

Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
"""

import os
import sys
import json
import urllib.request
import urllib.error

PROVIDERS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "providers")
HALCODE_DIR = os.path.expanduser("~/.halcode/providers")
KEYS_FILE = os.path.expanduser("~/.halcode/keys.env")

def load_keys_env():
    """Load API keys from ~/.halcode/keys.env into os.environ if not already present."""
    if os.path.isfile(KEYS_FILE):
        with open(KEYS_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, val = line.split("=", 1)
                    key = key.strip()
                    val = val.strip().strip("'\"")
                    if key and val:
                        os.environ[key] = val
                        # Map HC_ prefix to standard env var if standard is not set
                        if key.startswith("HC_"):
                            std_key = key[3:]
                            if std_key.endswith("_KEY"):
                                std_key = std_key[:-4] + "_API_KEY"
                            if std_key not in os.environ:
                                os.environ[std_key] = val

# Standard provider metadata templates
DEFAULT_SPECS = {
    "openrouter": {
        "name": "OpenRouter",
        "backend": "openai",
        "base_url": "https://openrouter.ai/api",
        "api_path": "/v1/chat/completions",
        "models_endpoint": "/v1/models",
        "auth": "bearer",
        "key_env": "OPENROUTER_API_KEY",
        "key_hint": "openrouter",
        "tier": 1,
        "default_model": "anthropic/claude-3.7-sonnet",
        "fallback_models": [
            {"id": "anthropic/claude-3.7-sonnet", "display": "Claude 3.7 Sonnet"},
            {"id": "deepseek/deepseek-r1", "display": "DeepSeek R1 Reasoning"},
            {"id": "deepseek/deepseek-chat", "display": "DeepSeek V3"},
            {"id": "google/gemini-2.0-flash-001", "display": "Gemini 2.0 Flash"},
            {"id": "meta-llama/llama-3.3-70b-instruct", "display": "Llama 3.3 70B"},
            {"id": "qwen/qwen-2.5-coder-32b-instruct", "display": "Qwen 2.5 Coder 32B"}
        ]
    },
    "anthropic": {
        "name": "Anthropic",
        "backend": "anthropic",
        "base_url": "https://api.anthropic.com",
        "api_path": "/v1/messages",
        "models_endpoint": "/v1/models",
        "auth": "x-api-key",
        "key_env": "ANTHROPIC_API_KEY",
        "key_hint": "anthropic",
        "tier": 1,
        "default_model": "claude-3-7-sonnet-20250219",
        "fallback_models": [
            {"id": "claude-3-7-sonnet-20250219", "display": "Claude 3.7 Sonnet"},
            {"id": "claude-3-5-sonnet-20241022", "display": "Claude 3.5 Sonnet"},
            {"id": "claude-3-5-haiku-20241022", "display": "Claude 3.5 Haiku"},
            {"id": "claude-3-opus-20240229", "display": "Claude 3 Opus"}
        ]
    },
    "openai": {
        "name": "OpenAI",
        "backend": "openai",
        "base_url": "https://api.openai.com",
        "api_path": "/v1/chat/completions",
        "models_endpoint": "/v1/models",
        "auth": "bearer",
        "key_env": "OPENAI_API_KEY",
        "key_hint": "openai",
        "tier": 1,
        "default_model": "gpt-4o",
        "fallback_models": [
            {"id": "gpt-4o", "display": "GPT-4o"},
            {"id": "gpt-4o-mini", "display": "GPT-4o Mini"},
            {"id": "o1", "display": "o1 (reasoning)"},
            {"id": "o3-mini", "display": "o3-mini (reasoning)"},
            {"id": "gpt-4.5-preview", "display": "GPT-4.5 Preview"},
            {"id": "gpt-4-turbo", "display": "GPT-4 Turbo"}
        ]
    },
    "gemini": {
        "name": "Google",
        "backend": "gemini",
        "base_url": "https://generativelanguage.googleapis.com",
        "api_path": "/v1beta/openai/chat/completions",
        "models_endpoint": "/v1beta/models",
        "auth": "bearer",
        "key_env": "GEMINI_API_KEY",
        "key_hint": "google_flash3",
        "tier": 1,
        "default_model": "gemini-2.0-flash",
        "fallback_models": [
            {"id": "gemini-2.0-flash", "display": "Gemini 2.0 Flash"},
            {"id": "gemini-2.0-flash-lite", "display": "Gemini 2.0 Flash Lite"},
            {"id": "gemini-2.0-pro-exp-02-05", "display": "Gemini 2.0 Pro Exp"},
            {"id": "gemini-1.5-pro", "display": "Gemini 1.5 Pro"},
            {"id": "gemini-1.5-flash", "display": "Gemini 1.5 Flash"}
        ]
    },
    "deepseek": {
        "name": "DeepSeek",
        "backend": "openai",
        "base_url": "https://api.deepseek.com",
        "api_path": "/v1/chat/completions",
        "models_endpoint": "/v1/models",
        "auth": "bearer",
        "key_env": "DEEPSEEK_API_KEY",
        "key_hint": "deepseek",
        "tier": 1,
        "reasoning_effort": "medium",
        "default_model": "deepseek-v4-pro",
        "fallback_models": [
            {"id": "deepseek-v4-pro", "display": "DeepSeek V4 Pro (thinking)"},
            {"id": "deepseek-v4-flash", "display": "DeepSeek V4 Flash (fast)"},
            {"id": "deepseek-v4-flash-vision-exp", "display": "DeepSeek V4 Vision (exp)"},
            {"id": "deepseek-chat", "display": "DeepSeek V3 (chat, legacy)"},
            {"id": "deepseek-reasoner", "display": "DeepSeek R1 (reasoner, legacy)"}
        ]
    },
    "groq": {
        "name": "Groq",
        "backend": "openai",
        "base_url": "https://api.groq.com",
        "api_path": "/openai/v1/chat/completions",
        "models_endpoint": "/openai/v1/models",
        "auth": "bearer",
        "key_env": "GROQ_API_KEY",
        "key_hint": "groq",
        "tier": 3,
        "default_model": "llama-3.3-70b-versatile",
        "fallback_models": [
            {"id": "llama-3.3-70b-versatile", "display": "Llama 3.3 70B Versatile"},
            {"id": "llama-3.1-8b-instant", "display": "Llama 3.1 8B Instant"},
            {"id": "deepseek-r1-distill-llama-70b", "display": "DeepSeek R1 Distill 70B"},
            {"id": "mixtral-8x7b-32768", "display": "Mixtral 8x7B"},
            {"id": "gemma2-9b-it", "display": "Gemma 2 9B"}
        ]
    },
    "xai": {
        "name": "xAI",
        "backend": "openai",
        "base_url": "https://api.x.ai",
        "api_path": "/v1/chat/completions",
        "models_endpoint": "/v1/models",
        "auth": "bearer",
        "key_env": "XAI_API_KEY",
        "key_hint": "xai",
        "tier": 1,
        "default_model": "grok-3",
        "fallback_models": [
            {"id": "grok-3", "display": "Grok 3"},
            {"id": "grok-3-mini", "display": "Grok 3 Mini"},
            {"id": "grok-2-1212", "display": "Grok 2"},
            {"id": "grok-2-vision-1212", "display": "Grok 2 Vision"}
        ]
    },
    "ollama": {
        "name": "Ollama (Local)",
        "backend": "openai",
        "base_url": "http://localhost:11434",
        "api_path": "/v1/chat/completions",
        "models_endpoint": "/v1/models",
        "auth": "none",
        "key_env": "OLLAMA_API_KEY",
        "key_hint": "ollama",
        "tier": 1,
        "default_model": "llama3.2:latest",
        "fallback_models": [
            {"id": "llama3.2:latest", "display": "Llama 3.2"},
            {"id": "qwen2.5-coder:latest", "display": "Qwen 2.5 Coder"},
            {"id": "deepseek-r1:latest", "display": "DeepSeek R1 Local"}
        ]
    }
}

def fetch_remote_models(provider_key, spec):
    # Lookup key from environment (checking both standard and HC_ prefixed names)
    key_var = spec["key_env"]
    hc_var = f"HC_{key_var.replace('_API_', '_')}"
    api_key = os.getenv(key_var) or os.getenv(hc_var) or os.getenv(f"HC_{key_var}") or ""

    base_url = spec["base_url"].rstrip("/")
    endpoint = spec.get("models_endpoint", "/v1/models")
    url = base_url + endpoint

    headers = {}
    if spec["name"] == "Google" and api_key:
        headers["x-goog-api-key"] = api_key
        url += f"?key={api_key}"
    elif spec["auth"] == "bearer" and api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    elif spec["auth"] == "x-api-key" and api_key:
        headers["x-api-key"] = api_key
        headers["anthropic-version"] = "2023-06-01"

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            if resp.status == 200:
                data = json.loads(resp.read().decode('utf-8'))
                models_raw = data.get("data", []) or data.get("models", [])
                if isinstance(models_raw, list) and len(models_raw) > 0:
                    model_list = []
                    for m in models_raw:
                        mid = m.get("id") or m.get("name") if isinstance(m, dict) else str(m)
                        if mid:
                            display_name = m.get("name", mid) if isinstance(m, dict) else mid
                            model_list.append({"id": mid, "display": display_name})
                    print(f"  [+] Live fetched {len(model_list)} models from {url}")
                    return model_list
    except Exception as e:
        print(f"  [-] Could not fetch live models for {spec['name']} ({url}): {e}", file=sys.stderr)

    return spec.get("fallback_models", [])

def main():
    load_keys_env()
    os.makedirs(PROVIDERS_DIR, exist_ok=True)
    os.makedirs(HALCODE_DIR, exist_ok=True)

    print("[+] Aggregating HalCode9000 provider configurations...")

    for key, spec in DEFAULT_SPECS.items():
        print(f"[*] Processing provider: {spec['name']}...")
        fetched_models = fetch_remote_models(key, spec)
        
        provider_data = {
            "name": spec["name"],
            "backend": spec["backend"],
            "base_url": spec["base_url"],
            "api_path": spec["api_path"],
            "models_endpoint": spec["models_endpoint"],
            "auth": spec["auth"],
            "key_env": spec["key_env"],
            "key_hint": spec["key_hint"],
            "tier": spec["tier"],
            "models": fetched_models,
            "default_model": spec["default_model"]
        }
        if "reasoning_effort" in spec:
            provider_data["reasoning_effort"] = spec["reasoning_effort"]

        # Write to repo providers/
        repo_file = os.path.join(PROVIDERS_DIR, f"{key}.json")
        with open(repo_file, "w", encoding="utf-8") as f:
            json.dump(provider_data, f, indent=2)

        # Write to ~/.halcode/providers/
        user_file = os.path.join(HALCODE_DIR, f"{key}.json")
        with open(user_file, "w", encoding="utf-8") as f:
            json.dump(provider_data, f, indent=2)

        print(f"  -> Saved {repo_file} ({len(fetched_models)} models)")

    print("[+] Done! Provider specs successfully generated.")

if __name__ == "__main__":
    main()
