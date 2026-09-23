<h1 align="center">BO2Z-Offline</h1>

<p align="center"><strong>Made by J_axon</strong></p>

<p align="center">
  <img src="assets/FullCoverBO2-Offline.png" width="800" alt="Call of Duty: Black Ops II Zombies Offline cover">
</p>

BO2Z-Offline lets owners of the original Steam **Call of Duty: Black Ops II Zombies** play Zombies without an Internet connection. Its dedicated launcher starts the unmodified game through Steam and attaches a local service backend. It does not replace `t6zm.exe`, Steam files, audio DLLs, or graphics DLLs. This is a Zombies offline-play mod, not the separate BO2 VR project.

> **Steam Offline Mode is highly recommended** before selecting **Play Offline**. BO2Z-Offline can also launch while Steam is online, but the game's own online behavior can be less predictable in some situations. Steam must still be installed, and your account must own the game.

## Support the project

BO2Z-Offline is completely free, and donating is entirely optional. Nothing is locked behind payment and you never need to donate to download, use, or modify the mod. If you enjoy it and would like to support future work, you can help on Ko-fi:

### [ko-fi.com/j_axon](https://ko-fi.com/j_axon)

## What happens to my rank and progress?

The first offline launch starts a **new, separate offline save**. Your online rank, banked points, and other online progress may therefore appear to be gone while you are playing offline. They have **not** been erased or transferred. Offline play has its own rank, stats, bank/locker data, public profile, and local leaderboard records. That progress is saved between offline sessions under `BO2Z-Offline/data/offline-profiles/<SteamID>/`.

Launching Zombies normally through Steam does **not** load BO2Z-Offline. Your regular online profile and rank appear again when the game's online service is available. Neither profile is merged into the other, and offline progress is not uploaded to your online account. Different Steam accounts receive separate offline saves.

Back up your complete `offline-profiles` folder if you want to preserve offline progress. Exit matches normally so BO2 can finish saving. Do not share that folder, the mod log, or dumps with a public release.

## Requirements

- Windows and an installed, owned Steam copy of Black Ops II Zombies.
- The supported original `t6zm.exe` build. Its SHA-256 is `F6F7104AF2BD0C2B931EA1E739969E85845E499AFFBF686144C6B490B344AB1E`.
- The complete BO2Z-Offline release files, including the required publisher resources in `BO2Z-Offline/data/pub/`.

No VR headset, OpenXR runtime, Plutonium installation, executable replacement, or Steam API replacement is required. Other versions of `t6zm.exe` are not claimed compatible.

> **Public-release status:** This source repository deliberately excludes the nine BO2 publisher resources that the tested offline build uses. They are not present as loose files in a stock Steam installation, and no complete public download is provided here until there is a suitable, authorized way to supply them. Do not treat GitHub's source ZIP as a ready-to-play release.

## Install with a complete authorized package

1. Close Black Ops II Zombies.
2. Obtain and extract a complete BO2Z-Offline package from a source authorized to provide the required publisher resources. Do not download GitHub's automatic **Source code** ZIP expecting a ready-to-play package; source archives do not contain those resources.
3. In Steam, right-click **Call of Duty: Black Ops II - Zombies**, choose **Manage > Browse local files**, and locate `t6zm.exe`.
4. Open the release package's `Copy into Black Ops II folder` directory. Copy its **`BO2Z-Offline-Launcher.exe`** and entire **`BO2Z-Offline`** folder beside `t6zm.exe`. Keep the `data/pub` contents together with the DLL.
5. Recommended: put Steam into **Offline Mode**. Open `BO2Z-Offline-Launcher.exe`, leave **Offline Status** on if you want the small **In Offline Mode** text in the upper-left, and select **Play Offline**. The toggle changes only that visual label, not saves or connectivity.
6. Use BO2's normal menus to choose Solo and start a Zombies match. To play with your original online profile later, close Zombies and launch it normally through Steam instead of the BO2Z-Offline launcher.

The launcher checks the original game's executable hash and required files, asks Steam to start the official Zombies AppID, attaches the local backend, and displays an error if setup fails. The status label is a separate click-through launcher overlay; it does not hook the game's renderer. The launcher remains running while the game is open when that label is enabled.

Example installation layout:

```text
Call of Duty Black Ops II/
├── t6zm.exe                         (original Steam executable)
├── BO2Z-Offline-Launcher.exe
└── BO2Z-Offline/
    ├── BO2Z-Offline.dll
    └── data/
        ├── pub/                     (required publisher resources)
        └── offline-profiles/        (created on first offline play; never bundled)
```

If BO2Z-Offline fails to launch, confirm that `t6zm.exe` is the supported original build, `data/pub` is complete, Steam is installed, and Zombies is not already running. The backend writes `BO2Z-Offline/BO2Z-Offline.log`; review it before sharing because logs can contain account or system details.

## Build from source

Building requires Windows, PowerShell 7, CMake 3.22 or later, and Visual Studio 2022 with the **Desktop development with C++** workload and the Windows SDK. Build for **Win32/x86**, not x64. The repository includes LibTomCrypt and zlib source under their own licenses; it does not include the BO2 executable or private user saves.

```powershell
git clone https://github.com/Ghostx10742/BO2Z-Offline.git
Set-Location .\BO2Z-Offline
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Successful builds produce `build/Release/BO2Z-Offline-Launcher.exe` and `build/Release/BO2Z-Offline.dll`. To make a **private local** install layout when you already have a complete, authorized copy of the tested publisher files, run:

```powershell
.\tools\package-local.ps1 -PublisherDir 'C:\path\to\your\existing\pub'
```

The script verifies the nine expected resources and writes `dist/local-install/Copy into Black Ops II folder/`. It copies only the allowlisted resources, never profiles or logs. Do not commit or publicly upload that local package without resolving redistribution rights. The launcher will reject an incomplete `data/pub` set.

The repository's source build omits the abandoned in-game renderer indicator; the launcher owns the status overlay. Automated tests cover the backend, profile isolation, and launcher bootstrap, but they do not substitute for a live Zombies play test.

## Scope and compatibility

BO2Z-Offline targets **Zombies offline solo play**. It does not promise multiplayer LAN, public matchmaking, cloud synchronization, global leaderboards, or anti-cheat compatibility. Online rank and offline rank remain intentionally separate. The mod is tied to the supported original executable build; a game update may require fresh verification.

## License, reuse, and credits

The original BO2Z-Offline code is released under the [Apache License 2.0](LICENSE). You may fork this repository or use the code in your own project under that license. When distributing modified or reused work, keep the license and [NOTICE](NOTICE), identify your changes, clearly credit **J_axon** as the original creator, and link to the [original BO2Z-Offline repository](https://github.com/Ghostx10742/BO2Z-Offline). A GitHub fork is welcome but is not required by Apache 2.0.

All creation and direction credits go to **J_axon**. Special thanks to tester **obesikaas**.

LibTomCrypt and zlib retain their own licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Call of Duty, Black Ops II, and their game artwork and data belong to their respective owners and are not licensed by this project's Apache 2.0 license. BO2Z-Offline is an independent fan project and is not affiliated with or endorsed by Activision or Treyarch.

## AI disclosure

AI was used during the development of this project, mainly for revisions, inquiries, and things I just did not know. This does not mean the mod was fully AI-made, but rather that AI was used as part of the development process. I wanted to disclose this for people who may have a problem with AI being involved and may not want anything to do with it. Even though I disagree with your view on AI, I still respect your opinion on the subject.
