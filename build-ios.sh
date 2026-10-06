#!/bin/bash
# Build a signed PPSSPP for iPhone/iPad with our own team and bundle ID.
#
# Uses the IOS_APP_STORE CMake mode, which is the one set up for normal Xcode
# code signing (b-ios.sh is for jailbroken devices and signs with ldid).
# Team and bundle ID are passed to CMake (PPSSPP_BUNDLE_IDENTIFIER is our own addition).
#
# Usage: ./build-ios.sh [--unsigned] [--debug]
#   --unsigned  compile only, skip code signing
#   --debug     Debug configuration instead of Release
#
# Output: build-ios-signed/<Config>-iphoneos/PPSSPP.app
# To open the project in Xcode instead: open build-ios-signed/PPSSPP.xcodeproj

set -e

TEAM_ID=F2R8FQ3WNK
BUNDLE_ID=com.darrenhuang.ppsspp
CONFIG=Release
SIGN=1
BUILD_DIR=build-ios-signed

for arg in "$@"; do
	case "$arg" in
		--unsigned) SIGN=0 ;;
		--debug) CONFIG=Debug ;;
		*) echo "Unknown option: $arg"; exit 1 ;;
	esac
done

cd "$(dirname "$0")"
mkdir -p "$BUILD_DIR"

# git-version.cpp is listed as a source file, so Xcode needs it before any script phase runs.
echo "const char *PPSSPP_GIT_VERSION = \"$(git describe --always)\";" > "$BUILD_DIR/git-version.cpp"
echo "#define PPSSPP_GIT_VERSION_NO_UPDATE 1" >> "$BUILD_DIR/git-version.cpp"

cmake -S . -B "$BUILD_DIR" -GXcode \
	-DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/ios.cmake \
	-DIOS_APP_STORE=ON \
	-DDEVELOPMENT_TEAM_ID="$TEAM_ID" \
	-DPPSSPP_BUNDLE_IDENTIFIER="$BUNDLE_ID"

XCODE_ARGS=(
	-project "$BUILD_DIR/PPSSPP.xcodeproj"
	-scheme PPSSPP
	-sdk iphoneos
	-configuration "$CONFIG"
	-quiet
)

if [ "$SIGN" = 1 ]; then
	xcodebuild "${XCODE_ARGS[@]}" -allowProvisioningUpdates build
else
	xcodebuild "${XCODE_ARGS[@]}" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
fi

APP=$(find "$BUILD_DIR" -maxdepth 3 -type d -path "*$CONFIG-iphoneos/PPSSPP.app" | head -1)
echo "== Built: $APP"
if [ "$SIGN" = 1 ]; then
	codesign -dv "$APP" 2>&1 | grep -E "Identifier|TeamIdentifier|Authority=Apple Development"
	echo "Install on a connected device with:"
	echo "  xcrun devicectl device install app --device <device-id> \"$APP\""
fi
