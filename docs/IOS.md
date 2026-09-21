# iOS Builds

The default IPA packaging target is `ios-native-metal-release`. It uses Plume's
native Metal command queues and pipelines, without linking MoltenVK. Existing
SPIR-V shader assets are translated to MSL with SPIRV-Cross before Metal compiles
them. The original installer and on-device game files are still used.

## Build an IPA

Use macOS with Xcode selected as the active developer directory, CMake, Ninja,
and the initialized submodules. Supply your own compatible game files as
described by the packaging script (`ISO_PATH`, `UPDATE_PATH`, or `XEXP_PATH`),
or place the extracted files in `UnleashedRecompLib/private`.

```sh
bash tools/package_ios_ipa.sh
```

The output is `out/ipa/UnleashedRecompiled.ipa`. Without a provisioning profile,
use a sideloading tool that signs the IPA on import. For an already provisioned
device build, provide matching signing settings:

```sh
CODESIGN_IDENTITY="Apple Development: ..." \
MOBILEPROVISION="/absolute/path/profile.mobileprovision" \
ENTITLEMENTS="/absolute/path/entitlements.plist" \
BUNDLE_IDENTIFIER="your.provisioned.bundle.identifier" \
bash tools/package_ios_ipa.sh
```

Keep the installed bundle identifier when updating to preserve its game data.
The provisioning profile must include the device and match the identifier and
entitlements. Native Metal requires Metal 3 buffer GPU addresses (iOS 16 or
later on compatible hardware).

The current device-tested IPA was built with Xcode 27 and targets iOS/iPadOS
27.0. Without an explicit `CMAKE_OSX_DEPLOYMENT_TARGET`, builds target the
selected SDK version. Supporting an older OS requires rebuilding the app and
its dependencies with a matching deployment target and testing that OS.

## Incremental Build

After the initial packaging/build has created the host tools and shader cache:

```sh
export VCPKG_ROOT="$PWD/thirdparty/vcpkg"
bash tools/apply_ios_submodule_patches.sh
cmake --preset ios-native-metal-release
cmake --build out/build/ios-native-metal-release --target UnleashedRecomp -j 8
```

`tools/patches/plume-ios-renderers.patch` contains both iOS renderer adaptations;
`tools/patches/sdl-ios-scene-lifecycle.patch` adds the SDL scene lifecycle needed
by current iOS SDKs. The patch script is idempotent and refuses conflicting
submodule changes.

## Vulkan Fallback

```sh
IOS_PRESET=ios-device-release bash tools/package_ios_ipa.sh
```

This separate preset uses Vulkan through MoltenVK. It does not overwrite the
native build directory, but both packaging commands use the same output IPA
path; archive the previous IPA before switching.

## Device Checks

- Confirm `[iOS video] backend=Native Metal` in the launch log.
- Check startup, installer, title screen, and a playable 3D level.
- Check touch input in landscape and after foregrounding the app.
- Use `MTL_DEBUG_LAYER=1` for validation only, not performance comparisons.
- `UNLEASHED_IOS_PROFILE=1` enables sampled frame statistics. Metal GPU timestamp
  queries are not implemented yet, so its reported GPU milliseconds are not a
  valid GPU timing measurement.

### Suspend/Resume Regression

With the console attached, switch from a playable scene to Settings or the Home
Screen, wait at least ten seconds, then return without terminating the app.
Repeat during loading and while holding a touch control. Verify:

- The process survives and frames continue after each return.
- The log pairs `Metal lifecycle: suspended (submissions scheduled)` with
  `Metal lifecycle: resumed`, without background-permission or ignored-submission
  errors. An FPS sample spanning time in the background will be artificially low.
- Sticks/buttons return to neutral after suspension, and fresh touches respond.
- Also exercise screen lock/unlock and the installer on the target OS/device.

The Metal submission barrier covers rendering, drawable acquisition, and
presentation. It schedules each queue's outstanding tail before returning from
the deactivation notification, following Apple's background GPU requirements.
The UIKit thread must continue pumping events while the installer is suspended.

### Touch Controls

The overlay uses Xbox 360-style button artwork with offset sticks and a cross
D-pad. The bottom-center green X toggles the overlay; the small arrow buttons
beside it are Back and Start. LT/RT are separate full-pressure digital triggers,
not aliases of LB/RB. Test simultaneous stick/button input, independent shoulder
and trigger presses, and hiding/restoring the overlay. Loading/cutscene black
bars must remain behind the touch controls.
