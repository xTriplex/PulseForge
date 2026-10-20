#!/bin/bash
set -euo pipefail

BRIDGE="${RUNNER_TEMP:?RUNNER_TEMP must be set}/PFAutomationPermission.framework"
mkdir -p "$BRIDGE/Versions/A/Headers" "$BRIDGE/Versions/A/Resources/BridgeSupport"
cp Tests/Platform/MacOSAutomationPermissionBridge.h "$BRIDGE/Versions/A/Headers/"
METADATA="$BRIDGE/Versions/A/Resources/BridgeSupport/PFAutomationPermission.bridgesupport"
cat > "$BRIDGE/Versions/A/Resources/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.pulseforge.test.automation-permission</string>
  <key>CFBundleExecutable</key><string>PFAutomationPermission</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleVersion</key><string>1</string>
</dict></plist>
EOF

clang -arch x86_64 -arch arm64 -fobjc-arc -fmodules -dynamiclib \
	-Wl,-install_name,@rpath/PFAutomationPermission.framework/Versions/A/PFAutomationPermission \
	-framework Foundation -framework ApplicationServices \
	Tests/Platform/MacOSAutomationPermissionBridge.m \
	-o "$BRIDGE/Versions/A/PFAutomationPermission" 2>&1 |
	tee "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"
lipo -archs "$BRIDGE/Versions/A/PFAutomationPermission" |
	tee -a "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"

ln -s A "$BRIDGE/Versions/Current"
ln -s Versions/Current/Headers "$BRIDGE/Headers"
ln -s Versions/Current/Resources "$BRIDGE/Resources"
ln -s Versions/Current/PFAutomationPermission "$BRIDGE/PFAutomationPermission"
ln -s Versions/Current/Resources/Info.plist "$BRIDGE/Info.plist"

if command -v gen_bridge_metadata >/dev/null 2>&1; then
	gen_bridge_metadata -f "$BRIDGE" -o "$METADATA" 2>&1 |
		tee -a "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"
	echo "Generated JXA BridgeSupport metadata from the built framework and public header."
else
	cp Tests/Platform/PFAutomationPermission.bridgesupport "$METADATA"
	echo "gen_bridge_metadata is unavailable; using the checked-in fixed scalar ABI metadata."
fi

for symbol in PFProbeSystemEventsDescriptor PFReadSystemEventsNativeAddress \
	PFTypeWildCardEventClass PFTypeWildCardEventID PFDetermineSystemEventsPermission; do
	if ! nm -gU "$BRIDGE/Versions/A/PFAutomationPermission" | grep -Eq "[[:space:]]_${symbol}$"; then
		echo "Missing exported permission bridge symbol: $symbol" >&2
		exit 1
	fi
done

test -s "$METADATA"
python3 - "$METADATA" Tests/Platform/MacOSAutomationPermissionBridge.h <<'PY'
import re
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
with open(sys.argv[2], encoding="utf-8") as header_file:
	header = header_file.read()
expected = set(re.findall(r"int32_t\s+(PF\w+)\s*\(void\)\s*;", header))
functions = root.findall("function")
actual = {function.attrib.get("name") for function in functions}
if root.tag != "signatures" or root.attrib.get("version") != "1.0":
	raise SystemExit("Invalid BridgeSupport signatures document")
if actual != expected:
	raise SystemExit(f"BridgeSupport function set differs from header: expected={sorted(expected)}, actual={sorted(actual)}")
for function in functions:
	retval = function.find("retval")
	return_types = [] if retval is None else [retval.attrib[key] for key in ("type", "type64") if key in retval.attrib]
	if function.findall("arg") or not return_types or any(value != "i" for value in return_types):
		raise SystemExit(f"Unexpected ABI for {function.attrib.get('name')}: {ET.tostring(function, encoding='unicode')}")
print(f"Verified BridgeSupport int32_t(void) ABI for {len(functions)} exported functions.")
PY

export PULSEFORGE_MACOS_AUTOMATION_BRIDGE="$BRIDGE"
if [[ -n "${GITHUB_ENV:-}" ]]; then
	echo "PULSEFORGE_MACOS_AUTOMATION_BRIDGE=$BRIDGE" >> "$GITHUB_ENV"
fi
