# Library Management

This project uses a configuration file to manage Arduino libraries, ensuring consistent dependencies across local development and CI/CD environments.

> **Note:** The template starts with no libraries configured. Example libraries (`WiFi` and `ArduinoJson`) are provided as comments in `arduino-libraries.txt`. Uncomment or add libraries as needed for your project.

## Configuration File

**`arduino-libraries.txt`** - Contains the list of required Arduino libraries

### Format

```
# Comments start with #
LibraryName
AnotherLibrary@1.2.3  # You can specify versions
"Library With Spaces"
```

- One library per line
- Library names must match `arduino-cli` library names exactly
- Optional version specification: `LibraryName@version`
- Lines starting with `#` are comments
- Empty lines are ignored

## Using the Library Management Script

The `library.sh` script provides convenient commands for managing libraries:

### Search for Libraries

```bash
./library.sh search <keyword>
```

Example:
```bash
./library.sh search mqtt
./library.sh search "adafruit sensor"
```

### Add a Library

Adds a library to `arduino-libraries.txt` and installs it immediately:

```bash
./library.sh add <library_name>
```

Examples:
```bash
./library.sh add PubSubClient
./library.sh add "Adafruit GFX Library"
./library.sh add ArduinoJson@6.21.3
```

### Remove a Library

Removes a library from `arduino-libraries.txt` (doesn't uninstall):

```bash
./library.sh remove <library_name>
```

### List Configured Libraries

Shows all libraries defined in `arduino-libraries.txt`:

```bash
./library.sh list
```

### Install All Libraries

Installs all libraries from `arduino-libraries.txt`:

```bash
./library.sh install
```

This is automatically called by `setup.sh`.

### Show Installed Libraries

Lists all currently installed libraries:

```bash
./library.sh installed
```

## Workflow Integration

### Local Development

1. Search for a library:
   ```bash
   ./library.sh search "sensor library"
   ```

2. Add it to your project:
   ```bash
   ./library.sh add "Adafruit BME280 Library"
   ```

3. The library is automatically installed and added to `arduino-libraries.txt`

4. Commit `arduino-libraries.txt`:
   ```bash
   git add arduino-libraries.txt
   git commit -m "Add BME280 sensor library"
   ```

### CI/CD (GitHub Actions)

The GitHub workflow automatically:
1. Runs `setup.sh` during environment setup
2. `setup.sh` reads `arduino-libraries.txt` and installs all listed libraries
3. Libraries are cached for faster subsequent builds

No additional workflow configuration needed!

## Manual Library Management

You can also edit `arduino-libraries.txt` directly:

```bash
# Edit the file
nano arduino-libraries.txt

# Install the updated libraries
./library.sh install
```

## Finding Library Names

To find the exact library name for `arduino-cli`:

```bash
./library.sh search <keyword>
```

Or visit: https://www.arduinolibraries.info/

## Example Workflow

```bash
# Initial setup
./setup.sh

# Later, you need an MQTT library
./library.sh search mqtt
# Shows: PubSubClient by Nick O'Leary

# Add it to your project
./library.sh add PubSubClient

# Verify it's configured
./library.sh list

# Commit the change
git add arduino-libraries.txt
git commit -m "Add MQTT support via PubSubClient"
git push

# GitHub Actions will automatically install PubSubClient during CI
```

## Troubleshooting

### Library Not Found

If `./library.sh add LibraryName` fails:
1. Search for the exact name: `./library.sh search keyword`
2. Check on https://www.arduinolibraries.info/
3. Verify the library is available for ESP32

### Version Conflicts

Specify exact versions in `arduino-libraries.txt`:
```
ArduinoJson@6.21.3
PubSubClient@2.8.0
```

### Library Already Installed

If you manually installed a library before, it won't be in `arduino-libraries.txt`. To add it:

```bash
./library.sh add LibraryName
```

This updates the config without reinstalling.
