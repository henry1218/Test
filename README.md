# WPTG GPU metrics addon

`gpu_metrics.cc` 的單一真相。這個 repo 只做兩件事：在 GitHub Actions 上把它編成 Windows x64 的 N-API
二進位檔，並把結果上傳成 workflow artifact。它不驗證產物的完整性 —— 驗證在 client 端進行；
artifact 由 GitHub 暫存 90 天後自動清除，不是永久保存。

## 這顆 addon 是什麼

一個 Windows-only 的 N-API 模組，對 JS 露出 `sample({ pids })` 與 `dispose()` 兩個函式。它透過 PDH 的
`GPU Engine` 與 `GPU Process Memory` 計數器取得 per-process 的 GPU 引擎使用率與顯存，並以 DXGI 列舉
adapter，把 LUID 對應到 PCI vendor/device ID。Electron 與 Chromium 沒有等價的 API，因此必須是原生模組。

## 建置

只有 GitHub Actions 建置，開發者不需要安裝 Visual Studio 或 Windows SDK。

1. 到 Actions 頁面選 **Build Windows x64 prebuild**
2. 點 **Run workflow**，在 `electronTarget` 填入目標 Electron 版本，例如 `44.3.0`
3. 完成後到該次 workflow run 的 **Artifacts** 區塊下載 `gpu-metrics-electron-<版本>-win32-x64`

這個 artifact 沒有另外打包成 zip——上傳的是 `dist/payload/` 整個資料夾，GitHub 下載時會自動包成 zip，
解開後結構為：

    win32-x64/gpu-metrics.node
    manifest.json

artifact 保留 90 天（`retention-days: 90`），過期後 GitHub 會自動刪除，無法回頭再下載同一次建置的產物，
必須重新 dispatch 一次。

## 版號怎麼決定

repo 內不儲存 Electron 版號。要哪個版本就 dispatch 哪個版本，因此**不可能在不重新編譯的情況下產生一份
標示新版號的 manifest**。這是刻意的設計。

每次 dispatch 都是獨立的 workflow run、獨立的 artifact，同一個 Electron 版本要重出幾次都可以，
不會有「名稱已存在」需要手動處理的情況——這也是拿掉 GitHub Release 之後，原本 tag 唯一性設計跟著
一起消失的部分。

## manifest

`manifest.json` 只有一個欄位：

| 欄位 | 意義 |
| --- | --- |
| `electronTarget` | 編譯目標的 Electron 版本 |

    {
        "electronTarget": "44.3.0"
    }

這個值也出現在 artifact 名稱裡，但一旦解壓後把資料夾複製到別處，名稱就不在了，manifest 是唯一跟著
檔案走的紀錄。

manifest 刻意不帶 schema 版號。欄位只有一個時，consumer 直接檢查 `electronTarget` 在不在、格式對不對，
比先讀版號再決定怎麼解析更簡單也更可靠。代價是往後**只能做加法** —— 要擴充就加 optional 欄位，
不改名、不刪除，這樣舊 consumer 讀新 manifest 永遠不會壞。真的需要破壞性變更時，得另外安排一次
與 consumer 同步的切換，沒有版號可以擋。

產物的完整性（大小、sha256、PE machine）不記在 manifest 裡 —— 建置時 `assertPe` 會驗 PE 標頭，
sha256 印在 workflow log，實際驗證在 client 端進行。

## 本機執行

`scripts/build.js` 在非 Windows 上會立即失敗，且平台檢查排在清空 `dist/` 之前，因此不會誤刪任何東西。

在 Windows 上可以本機編譯（例如開發時測試 `gpu_metrics.cc` 的改動）：不需要任何環境變數。
但本機建置只做到產出 `dist/payload/`，實際上傳成 artifact 仍只透過 GitHub Actions 的 workflow 完成。

單元測試不需要 Windows：

    npm ci
    npm test

測試涵蓋參數解析、PE 標頭驗證，以及 manifest 組裝。

## 新增架構目標

`scripts/build.js` 的 `TARGETS` 是唯一修改點。ia32 的 PE machine 是 `0x014c`，arm64 是 `0xaa64`。
client 端的驗證也有一份對應的 `TARGETS`，兩邊要一起改。

## 消費端

`wptg-electron-multi-table` 的 `ts/modules/diagnostics-module/native/gpu-metrics/prebuilds/`。
升版程序記錄在該目錄的 README——但那份文件如果還是照著「去 Release 頁面抓某個 tag」寫的，
需要跟著改成「去對應 workflow run 的 Artifacts 區塊下載，且 90 天內要抓」，這邊沒有一併改。

**manifest 欄位已變動，client 端需同步確認。** 原本的 `schemaVersion`、`source.commit` 以及
`artifacts["win32-x64"]`（含 `sha256`、`sizeBytes`、`peMachine`）都已移除，現在只剩 `electronTarget`。
client 端如果有讀這些欄位或做 `schemaVersion` 檢查，會拿到 `undefined`，需要一併調整；原本依賴
manifest 內 `sha256` 做完整性驗證的部分，得改用其他來源。
