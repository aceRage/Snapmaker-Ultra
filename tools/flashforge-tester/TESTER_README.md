# Testing Flashforge Creator 5 support — and sending us the logs

Thanks for helping test! This takes about 10 minutes. No technical knowledge needed.

## One-time setup

1. Install **Flash Studio Desktop** from flashforge.com if you don't have it already
   (we borrow two connection files from it — we're not allowed to ship them ourselves).
2. Copy `setup-flashforge.cmd` into the Snapmaker Orca Ultra folder
   (the folder that contains `snapmaker-orca.exe`) and double-click it.
   It should end with "Done!".

## The test

1. Start Snapmaker Orca Ultra.
2. In the printer dropdown (top-left), select **Flashforge Creator 5** (any nozzle).
3. Open the **Device** tab. Wait ~30 seconds.
   - Does your printer appear in the Device List? (It must be on the same network.)
4. If it appears: click it, enter the check code if asked, and see whether the
   Device Status page shows temperatures / camera.
5. If you feel adventurous: slice the default cube and try **Print** to send it.
   Watch the printer — cancel the print at the machine if it starts.

It's completely fine if any step fails — that's exactly what the logs are for.

## Sending us the logs

1. In the app: **Help → Export Logs**, save the ZIP wherever is easy (Desktop).
2. Send us that one ZIP file, plus two sentences:
   - what you did,
   - what you expected vs. what happened.

That's everything. The ZIP contains only application and connection logs
(no personal files, no account credentials).
