# WPTG GPU metrics addon

`gpu_metrics.cc` 的單一真相。這個 repo 只做兩件事：在 GitHub Actions 上把它編成 Windows x64 的 N-API
二進位檔，並把結果發成 GitHub release。它不驗證產物的完整性，也不保存產物 —— 驗證在 client 端進行。

## 這顆 addon 是什麼

一個 Windows-only 的 N-API 模組，對 JS 露出 `sample({ pids })` 與 `dispose()` 兩個函式。它透過 PDH 的
`GPU Engine` 與 `GPU Process Memory` 計數器取得 per-process 的 GPU 引擎使用率與顯存，並以 DXGI 列舉
adapter，把 LUID 對應到 PCI vendor/device ID。Electron 與 Chromium 沒有等價的 API，因此必須是原生模組。

## 建置

只有 GitHub Actions 建置，開發者不需要安裝 Visual Studio 或 Windows SDK。

1. 到 Actions 頁面選 **Build Windows x64 prebuild**
2. 點 **Run workflow**，在 `electronTarget` 填入目標 Electron 版本，例如 `44.3.0`
3. 完成後會建立一個 release，tag 為 `electron-<版本>-<source commit 前 7 碼>`

release 帶有單一 asset `gpu-metrics-electron-<版本>-win32-x64.zip`，解開後結構為：

    win32-x64/gpu-metrics.node
    manifest.json

## 版號怎麼決定

repo 內不儲存 Electron 版號。要哪個版本就 dispatch 哪個版本，因此**不可能在不重新編譯的情況下產生一份
標示新版號的 manifest**。這是刻意的設計。

同一個 Electron 版本若因源碼變更需要重出，tag 會因 source commit 不同而唯一。tag 已存在時 CI 直接失敗，
不覆蓋已發佈的產物；要重出同一組合必須先手動刪除該 release。

## manifest

`manifest.json` 為 `schemaVersion: 2`，欄位：

| 欄位 | 意義 |
| --- | --- |
| `electronTarget` | 編譯目標的 Electron 版本 |
| `sourceSha256` | `binding.gyp` 與 `gpu_metrics.cc` 的串接指紋 |
| `sourceFiles` | 參與指紋計算的檔案與順序 |
| `source.repo` / `source.commit` / `source.releaseTag` / `source.runUrl` | 建置出處，取自 runner 環境變數 |
| `artifacts["win32-x64"]` | `path` / `sizeBytes` / `sha256` / `peMachine` |

Actions 的執行紀錄會過期，因此要核對一份 manifest 的真偽時，比對的對象是 release notes 而不是 `source.runUrl`。

## 本機執行

`scripts/build.js` 在非 Windows 上會立即失敗，且平台檢查排在清空 `dist/` 之前，因此不會誤刪任何東西。
即使在 Windows 上，缺少 `GITHUB_*` 環境變數時也會失敗 —— 本機建置不得產出看似可發佈的 manifest。

單元測試不需要 Windows：

    npm ci
    npm test

測試涵蓋參數解析、PE 標頭驗證、provenance 組裝，以及源碼指紋是否仍等於
`edeb66ad8739fcae5c8c17b29d74b843de932ad60b9e4750205e175f92920cb0`。**源碼變更時必須同步更新
`scripts/build.test.js` 裡的這個期望值**，否則測試會失敗。

## 新增架構目標

`scripts/build.js` 的 `TARGETS` 是唯一修改點。ia32 的 PE machine 是 `0x014c`，arm64 是 `0xaa64`。
client 端的驗證也有一份對應的 `TARGETS`，兩邊要一起改。

## 消費端

`wptg-electron-multi-table` 的 `ts/modules/diagnostics-module/native/gpu-metrics/prebuilds/`。
升版程序記錄在該目錄的 README。
