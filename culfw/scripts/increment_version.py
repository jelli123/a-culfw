import os
import re
import datetime

Import("env")

if env.GetOption("clean"):
    Return()

# Pfade
VERSION_FILE = os.path.join(env['PROJECT_DIR'], "version.h")
COUNTER_FILE = os.path.join(env['PROJECT_DIR'], ".build_number")


def get_base_version():
    """Liest BASE_VERSION aus version.h"""
    ver_str = "0.00"
    if os.path.exists(VERSION_FILE):
        with open(VERSION_FILE, 'r') as f:
            content = f.read()
            # Suche nach: #define BASE_VERSION "1.26"
            match = re.search(r'#define\s+BASE_VERSION\s+"(.*)"', content)
            if match:
                ver_str = match.group(1)
    return ver_str

def get_and_increment_build_number():
    # 1. GitHub Actions (NEU)
    if "GITHUB_RUN_NUMBER" in os.environ:
        return int(os.environ["GITHUB_RUN_NUMBER"])
        
    # 2. GitLab CI (Alt, kann bleiben)
    if "CI_PIPELINE_IID" in os.environ:
        return int(os.environ["CI_PIPELINE_IID"])

    # 3. Fallback: Lokal zählen (wie bisher)
    num = 1
    if os.path.exists(COUNTER_FILE):
        try:
            with open(COUNTER_FILE, 'r') as f:
                content = f.read().strip()
                if content.isdigit():
                    num = int(content) + 1
        except:
            pass

    # Speichern nur lokal sinnvoll
    with open(COUNTER_FILE, 'w') as f:
        f.write(str(num))
    return num

# --- MAIN ---

# 1. Daten sammeln
base_ver = get_base_version()

build_num = get_and_increment_build_number()
build_date = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

# 2. Finale Version bauen: "1.26" + "." + "42" -> "1.26.42"
full_version = f"{base_ver}.{build_num}"

# 3. Ausgabe für dich
print("\033[96m")
print("*******************************************")
print("* FIRMWARE VERSION UPDATE                 *")
print("*******************************************")
print(f"* Base Version: {base_ver}")
print(f"* Build Number: {build_num}")
print(f"* New Version:  {full_version}")
print(f"* Build Date:   {build_date}")
print("*******************************************")
print("\033[0m")

# 4. Injecting: Wir überschreiben das Makro VERSION für den Compiler
# Die Backslashes sind wichtig für C-Strings: \"1.26.42\"
env.Append(CPPDEFINES=[
    ("VERSION", f'\\"{full_version}\\"'),
    ("BUILD_NUMBER", f'\\"{build_num}\\"'),
    ("BUILD_DATE", f'\\"{build_date}\\"')
])
