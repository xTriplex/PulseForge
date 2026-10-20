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

clang -arch arm64 -fobjc-arc -fmodules -dynamiclib \
	-Wl,-install_name,@rpath/PFAutomationPermission.framework/Versions/A/PFAutomationPermission \
	-framework Foundation -framework ApplicationServices \
	Tests/Platform/MacOSAutomationPermissionBridge.m \
	-o "$BRIDGE/Versions/A/PFAutomationPermission" 2>&1 |
	tee "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"

ln -s A "$BRIDGE/Versions/Current"
ln -s Versions/Current/Headers "$BRIDGE/Headers"
ln -s Versions/Current/Resources "$BRIDGE/Resources"
ln -s Versions/Current/PFAutomationPermission "$BRIDGE/PFAutomationPermission"
ln -s Versions/Current/Resources/Info.plist "$BRIDGE/Info.plist"
cp Tests/Platform/PFAutomationPermission.bridgesupport "$METADATA"
echo "Packaged the checked-in Objective-C class metadata at the framework resource path."

if ! nm -gU "$BRIDGE/Versions/A/PFAutomationPermission" |
	grep -Fq '_OBJC_CLASS_$_PFAutomationPermissionBridge'; then
	echo "Missing exported Objective-C permission bridge class." >&2
	exit 1
fi
echo "Verified the exported Objective-C permission bridge class in the private framework."

{
	echo "Framework root: $BRIDGE"
	find "$BRIDGE" -maxdepth 5 -print -exec ls -ld {} \;
	echo "Framework Info.plist:"
	plutil -lint "$BRIDGE/Resources/Info.plist"
	plutil -p "$BRIDGE/Resources/Info.plist"
	echo "Framework executable:"
	file "$BRIDGE/PFAutomationPermission"
	lipo -archs "$BRIDGE/PFAutomationPermission"
	/usr/bin/otool -L "$BRIDGE/PFAutomationPermission"
	/usr/bin/codesign -dv --verbose=2 "$BRIDGE" 2>&1 || true
} 2>&1 | tee -a "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"

test -s "$METADATA"
python3 - "$METADATA" Tests/Platform/MacOSAutomationPermissionBridge.h <<'PY'
import re
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
with open(sys.argv[2], encoding="utf-8") as header_file:
	header = header_file.read()
expected = set(re.findall(r"-\s*\(int32_t\)(\w+)\s*;", header))
classes = [item for item in root.findall("class") if item.attrib.get("name") == "PFAutomationPermissionBridge"]
if root.tag != "signatures" or root.attrib.get("version") != "1.0" or len(classes) != 1:
	raise SystemExit("Invalid Objective-C BridgeSupport class metadata")
methods = classes[0].findall("method")
actual = {method.attrib.get("selector") for method in methods if method.attrib.get("class_method") != "true"}
if actual != expected or len(methods) != len(expected):
	raise SystemExit(f"BridgeSupport method set differs from header: expected={sorted(expected)}, actual={sorted(actual)}")
for method in methods:
	retval = method.find("retval")
	return_types = [] if retval is None else [retval.attrib[key] for key in ("type", "type64") if key in retval.attrib]
	if method.findall("arg") or not return_types or any(value != "i" for value in return_types):
		raise SystemExit(f"Unexpected class method ABI for {method.attrib.get('selector')}: {ET.tostring(method, encoding='unicode')}")
print(f"Verified Objective-C int32_t no-argument metadata for {len(methods)} class methods.")
PY

export PULSEFORGE_MACOS_AUTOMATION_BRIDGE="$BRIDGE"
if [[ -n "${GITHUB_ENV:-}" ]]; then
	echo "PULSEFORGE_MACOS_AUTOMATION_BRIDGE=$BRIDGE" >> "$GITHUB_ENV"
fi
