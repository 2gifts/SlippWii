/* SlippWii: "Check for Updates" (source/update.c).
 *
 * The Slippi Settings page's first row: asks GitHub for the latest release and, if it is newer than
 * this one, installs it on the card. Upstream Nintendont's own updater, which this header once
 * declared, is not in Slippi Nintendont.
 */
#ifndef __UPDATE_H__
#define __UPDATE_H__

/* The screens of it. Returns when the person is done; after an update was installed it goes back to
 * the Homebrew Channel instead (the app in memory is the old one). */
void SlippWii_Update(void);

/* tests (tools/update_wii_test.py): the launch argument "slippwii-update-test=URL;FOLDER", kept by
 * main.c. Only a test build acts on it (source/update_test.inc): there SlippWii_UpdateTest runs the
 * update by itself and returns 1. In every other build it returns 0 and does nothing. */
extern const char *SlippWiiUpdateTest;
int SlippWii_UpdateTest(void);

#endif
