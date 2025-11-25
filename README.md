# RTK GNSS Base Station using ESP32-S3 and Ublox ZED-F9P

Design and build a RTK GNSS Base Station to provide precise positioning service, which sends RTCM messages over a self-hosted NTRIP web server or an external Radio module, supports micro SD Card storage for post-processing purposes.

## Hardware

### KiCAD

Install:

```sh
sudo add-apt-repository ppa:kicad/kicad-9.0-releases
sudo apt update
sudo apt install kicad
```

Extensions:

- Parts from Espressif: <https://github.com/espressif/kicad-libraries>
- Parts from JLCPCB: <https://github.com/CDFER/JLCPCB-Kicad-Library>
- PCB Export to JLCPCB: <https://github.com/Bouni/kicad-jlcpcb-tools>
- PCB RF tools: <https://github.com/easyw/RF-tools-KiCAD>

Parts:

- JLCPCB Parts: <https://jlcpcb.com/parts/> which mainly is from LCSC Parts: <https://www.lcsc.com/>

  - Search for __Stock Type = In Stock__ and __Parts Type = Basic | Promotional Extended__ and __PCBA Type = Economic__

  - Tool to download symbols, footprints and 3D models from LCSC to KiCAD:

    <details>
    <summary>easyeda2kicad</summary>

    Homepage: <https://github.com/enduity/easyeda2kicad>

    Install:

    ``` sh
    cd hardware
    git clone https://github.com/enduity/easyeda2kicad
    cd easyeda2kicad
    python3 -m venv .venv
    source .venv/bin/activate
    pip install .
    ```

    Go back to the hardware project folder, create `libs` folder:

    ```sh
    cd ..
    mkdir -p libs
    ```

    Then download parts from LCSC, for example, ESP32-S3-WROOM-1-N16R8 has LCSCID=C2913202:

    ```sh esp32-s3-gnss-base-station/hardware (.venv)
    # ESP32-S3-WROOM-1-N16R8
    easyeda2kicad --lcsc_id C2913202 --full --output ./libs/LCSC --project-relative
    ```

    Add the LCSC libs into KiCAD projects:

    - Goto Preferences > Manage Symbol Libraries, in Project Specific Libs, add `${KIPRJMOD}/libs/LCSC.kicad_sym`
    - Goto Preferences > Manage Footprint Libraries, in Project Specific Libs, add `${KIPRJMOD}/libs/LCSC.pretty`

    </details>

### Schematic

| Part                                         | Link                                                                 |
| -------------------------------------------- | -------------------------------------------------------------------- |
| Espressif WiFi Module ESP32-S3-WROOM-1-N16R8 | https://jlcpcb.com/partdetail/3198300-ESP32_S3_WROOM_1N16R8/C2913202 |
| U-blox GNSS Receiver ZED-F9P-04B             | https://jlcpcb.com/partdetail/UBLOX-ZED_F9P04B/C5120257              |
| ShouHan Female Micro-B Connector             | https://jlcpcb.com/partdetail/SHOUHAN-MicroQTJ/C404968               |
| SOFNG MicroSD Card Push-Push Connector       | https://jlcpcb.com/partdetail/SOFNG-TF001B/C125617                   |
| XKB Slide Switches SS-12D10L5                | https://jlcpcb.com/partdetail/XKBConnection-SS12D10L5/C319012        |
