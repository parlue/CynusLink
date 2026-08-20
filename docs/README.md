# CynusLink Web Flasher

The manifest installs the complete ESP32-S3 image set: bootloader,
partition table, boot application selector and CynusLink firmware.

Upload this complete `docs` directory to the root of the CynusLink GitHub repository.

Then open:

Settings -> Pages

Select:

- Source: Deploy from a branch
- Branch: main
- Folder: /docs

After GitHub Pages deploys, the installer should be available at:

https://parlue.github.io/CynusLink/
