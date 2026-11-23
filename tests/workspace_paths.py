from pathlib import Path


def workspace_path(data, value):
    """Decode a library path from a persisted request, result or project."""
    if value.startswith("data:"):
        return data / value.removeprefix("data:")
    return Path(value)
