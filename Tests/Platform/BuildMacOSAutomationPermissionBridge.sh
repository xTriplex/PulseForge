#!/bin/bash
set -euo pipefail

BRIDGE="${RUNNER_TEMP:?RUNNER_TEMP must be set}/PFAutomationPermission.framework"
mkdir -p "$BRIDGE/Versions/A/Headers" "$BRIDGE/Versions/A/Resources"
cp Tests/Platform/MacOSAutomationPermissionBridge.h "$BRIDGE/Versions/A/Headers/"
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

if ! nm -gU "$BRIDGE/Versions/A/PFAutomationPermission" |
	grep -Fq '_OBJC_CLASS_$_PFAutomationPermissionBridge'; then
	echo "Missing exported Objective-C permission bridge class." >&2
	exit 1
fi
echo "Verified the exported Objective-C permission bridge class in the private framework."

export PULSEFORGE_MACOS_AUTOMATION_BRIDGE="$BRIDGE"
if [[ -n "${GITHUB_ENV:-}" ]]; then
	echo "PULSEFORGE_MACOS_AUTOMATION_BRIDGE=$BRIDGE" >> "$GITHUB_ENV"
fi
