# Hosted OLED and UART output

The website is at
<https://garden-guard.brian-ee2028.workers.dev/drone/GG01>.
Anyone with the link can view it. It contains two sections: the board's
actual 128×64 OLED image and its UART text. The website runs in the cloud and
opens even when the gateway laptop is off. Live outputs need the board and capture
relay; empty outputs stay empty until real data arrives.

The current host is the free Cloudflare Workers deployment. Its request and
database quotas apply to continuous streaming; see the limits in the
[Cloudflare guide](../web/command-centre-cloud/CLOUDFLARE.md). Stop the capture
relay after a demonstration rather than leaving it uploading all day.

## What runs where

```text
STM32 OLED framebuffer -> ST-Link read -> laptop HTTPS relay -> hosted OLED
STM32 USART1 -> ST-Link virtual COM port -> same relay -> hosted UART
```

The OLED image comes from the real framebuffer, rather than a browser
reconstruction of the game. UART lines come from serial reception or an
explicitly selected UART log, rather than generated telemetry summaries.
The page retains the last output and marks it stale or offline when capture
stops. A successful relay upload does not make an old frame or log line new.
Capture targets 30 frames per second. The relay uploads overlapping batches of
up to 20 real frames every 250 ms using a persistent HTTPS connection. The
browser polls every 250 ms and plays the captured timestamps through a 500 ms
jitter buffer. It never synthesizes intermediate game frames. Effective frame
rate depends on USB reads and Internet latency; the 4 November USB bench
measurement received 20.67 coherent frames per second. Terminal frames appear
immediately; stale/offline capture freezes the image.

The public page is read-only. The physical USER button still controls the
game. Live outputs travel over USB through the gateway laptop's Internet
connection.

## Start live updates

### Windows

Keep the board connected to its ST-Link USB port and close the native laptop
viewer so the relay can use ST-Link. The existing firmware can stay running;
the relay verifies its matching local ELF/BIN before reading any frames.

Install the Windows serial dependency once in the project virtual environment:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install pyserial
```

Start the website relay from PowerShell:

```powershell
.\tools\website.ps1 -Port COM3
```

The launcher finds the installed STM32CubeIDE tools under `C:\ST` and Git for
Windows. It prompts silently for the private relay token unless
`GARDEN_GUARD_RELAY_TOKEN` is already supplied to the process. Direct Windows
UART capture uses pyserial at 115200 8N1 with flow control disabled. The
`-TokenStdin` option is for a trusted launcher supplying the token over a
noninteractive input pipe; do not put a token in a command, file or Git.

Open <https://garden-guard.brian-ee2028.workers.dev/drone/GG01> on your
laptop or phone. Keep the laptop awake, online and connected to the board for
live updates. Ctrl+C stops the relay while the firmware continues running.

### macOS / POSIX

Connect the board's ST-Link USB port and run from the project directory:

```sh
python3 tools/cloud_relay.py --outputs-only --oled \
  --uart-port /dev/cu.usbmodemYOUR_PORT --token-prompt
```

Replace the serial device with the board's actual virtual COM port. This mode
does not need the local command-centre server. It reads the OLED framebuffer
without resetting or flashing the board. Its ELF/BIN verification requires
the existing `build/firmware/` image to match the firmware on the board; a
mismatch leaves capture offline with an explanatory console error.

If another terminal already receives UART, select its live log instead of
opening the serial port again:

```sh
python3 tools/cloud_relay.py --outputs-only --oled \
  --uart-log /path/to/live-uart.log --token-prompt
```

The default destination is the hosted website above. The relay asks for a
private upload token using a hidden prompt. The matching
`RELAY_TOKEN` is a secret runtime setting on the hosted Site. It belongs only
on the gateway and server; never place it in browser JavaScript, an NFC URL,
screenshots or Git. To replace a lost token, configure the same new random
value on the Site and the relay, then redeploy the existing saved version so
the server uses the new setting.

The relay uses verified HTTPS and keeps at most 100 UART lines in its upload.
The website address stays the same when the laptop's local IP changes. The cloud
website remains available after capture stops. When the board terminates, its
last outputs remain marked ended. Reset the board and restart capture to begin
a new session.

## NFC phone access

The firmware's default NFC URL is now:

```text
https://garden-guard.brian-ee2028.workers.dev/drone/GG01
```

To override the default explicitly on a future build:

```sh
make EXTRA_DEFINES='-DAPP_NFC_DASHBOARD_URL=\"https://garden-guard.brian-ee2028.workers.dev/drone/GG01\"'
```

Building does not change the physical board. Review the intended firmware,
then use the existing explicit flash workflow when the board is reconnected.
The firmware appends `?mission=0` and verifies the tag's
stored record. This link fits the firmware's 179-byte base-URL limit.

On 4 November the connected board programmed and read back the 76-byte NFC
record containing the earlier ChatGPT-hosted URL with `?mission=0`, with zero NFC errors.
`NFC READY` confirms the readback and RF wake succeeded. Tap the board's NFC
antenna and open the phone notification. The phone can use mobile data or any
Internet connection; live output still requires the USB gateway laptop. A
phone opening the link is a separate physical check from EEPROM verification.

## Hosting source

`web/command-centre-cloud/` contains the standalone Worker, embedded output page,
assets and database migration. The active Cloudflare deployment uses
`wrangler.jsonc`; see [Cloudflare deployment](../web/command-centre-cloud/CLOUDFLARE.md).
The original `.openai/hosting.json` Site ID is retained for the previous host.
Keep credentials in secure CLI storage or process memory, never in source.

The cloud upload endpoint requires the server-held token. Browser access to
state is public, and the public mission endpoint rejects writes. Wi-Fi
passwords and firmware binaries are never needed by the hosted website.
