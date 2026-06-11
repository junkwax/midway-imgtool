import re

with open(r'c:\Users\xbx\Workplace\midway-imgtool\platform\imgui_overlay.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Let's find matches of:
# void name(args) { ... }
# int name(args) { ... }
# etc.
pattern = r'(?:\n|\r|^)(?:static\s+)?(?:unsigned\s+)?(?:\w+(?:\s+\*+)?\s+)?(\w+)\s*\([^)]*\)\s*\{'
matches = re.finditer(pattern, content)

seen = set()
for match in matches:
    func_name = match.group(1)
    if func_name not in seen:
        seen.add(func_name)
        # print the signature line
        line_start = content.rfind('\n', 0, match.start()) + 1
        line_end = content.find('{', match.start())
        sig = content[line_start:line_end].strip()
        print(sig)
