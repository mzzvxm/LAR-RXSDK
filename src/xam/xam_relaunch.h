/**
 * @file        xam/xam_relaunch.h
 * @brief       Launch data of a title that reboots itself (private)
 */

#pragma once

namespace rex::xam {

struct XAMLoaderState;

/**
 * Keeps the launch flags and launch data of a title that is about to start itself again through
 * XamLoaderLaunchTitle, in a sidecar file next to the executable. The app restarts the process
 * (or the title), and the next title attach reads the file back, so XamLoaderGetLaunchData sees
 * what the title stored before the reboot.
 *
 * @param state The loader state of the title that is exiting.
 */
void StoreRelaunchData(const XAMLoaderState& state);

/**
 * Restores the launch flags and launch data StoreRelaunchData kept, then deletes the sidecar file
 * so a relaunch happens at most once. Does nothing when there is no sidecar file. Run when a title
 * attaches, before its guest code runs.
 *
 * @param state The loader state of the title that attaches.
 */
void RestoreRelaunchData(XAMLoaderState& state);

}  // namespace rex::xam
