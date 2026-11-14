"""Keep the executable low-LS program from a full live-store function scan."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
functions = json.loads((root / "live_01ee0_functions.json").read_text())
functions = [
    f for f in functions
    if 0xA0 <= int(f["start"]) < 0x2AAC and int(f["end"]) <= 0x2AAC
]
(root / "live_01ee0_code_functions.json").write_text(
    json.dumps(functions, indent=2) + "\n"
)
print(f"kept {len(functions)} low-LS functions")
