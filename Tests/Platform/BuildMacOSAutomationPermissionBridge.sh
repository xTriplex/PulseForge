#!/bin/bash
set -euo pipefail

BRIDGE="${RUNNER_TEMP:?RUNNER_TEMP must be set}/PFAutomationPermission.framework"
mkdir -p "$BRIDGE/Versions/A/Headers" "$BRIDGE/Versions/A/Resources/BridgeSupport"
cp Tests/Platform/MacOSAutomationPermissionBridge.h "$BRIDGE/Versions/A/Headers/"
cp Tests/Platform/PFAutomationPermission.bridgesupport \
	"$BRIDGE/Versions/A/Resources/BridgeSupport/PFAutomationPermission.bridgesupport"
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

if command -v gen_bridge_metadata >/dev/null 2>&1; then
	echo "gen_bridge_metadata is available; the fixed no-argument int32_t ABI uses checked-in metadata."
else
	echo "gen_bridge_metadata is unavailable; the fixed no-argument int32_t ABI uses checked-in metadata."
fi

clang -fobjc-arc -fmodules -dynamiclib \
	-Wl,-install_name,@rpath/PFAutomationPermission.framework/Versions/A/PFAutomationPermission \
	-framework Foundation -framework ApplicationServices \
	Tests/Platform/MacOSAutomationPermissionBridge.m \
	-o "$BRIDGE/Versions/A/PFAutomationPermission" 2>&1 |
	tee "$RUNNER_TEMP/macos-automation-permission-bridge-build.log"

for symbol in PFProbeSystemEventsDescriptor PFReadSystemEventsNativeAddress \
	PFTypeWildCardEventClass PFTypeWildCardEventID PFDetermineSystemEventsPermission; do
	if ! nm -gU "$BRIDGE/Versions/A/PFAutomationPermission" | grep -Eq "[[:space:]]_${symbol}$"; then
		echo "Missing exported permission bridge symbol: $symbol" >&2
		exit 1
	fi
done

test -s "$BRIDGE/Versions/A/Resources/BridgeSupport/PFAutomationPermission.bridgesupport"
python3 -c 'import xml.etree.ElementTree as ET,sys; root=ET.parse(sys.argv[1]).getroot(); assert root.tag == "signatures" and root.attrib.get("version") == "1.0"; funcs=root.findall("function"); assert len(funcs) == 5; assert all(f.find("retval") is not None and f.find("retval").attrib == {"type":"i"} and not f.findall("arg") for f in funcs)' \
	"$BRIDGE/Versions/A/Resources/BridgeSupport/PFAutomationPermission.bridgesupport"

ln -s A "$BRIDGE/Versions/Current"
ln -s Versions/Current/Headers "$BRIDGE/Headers"
ln -s Versions/Current/Resources "$BRIDGE/Resources"
ln -s Versions/Current/PFAutomationPermission "$BRIDGE/PFAutomationPermission"
ln -s Versions/Current/Resources/Info.plist "$BRIDGE/Info.plist"

export PULSEFORGE_MACOS_AUTOMATION_BRIDGE="$BRIDGE"
if [[ -n "${GITHUB_ENV:-}" ]]; then
	echo "PULSEFORGE_MACOS_AUTOMATION_BRIDGE=$BRIDGE" >> "$GITHUB_ENV"
fi
