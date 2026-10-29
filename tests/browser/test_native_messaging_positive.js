#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager -- Positive Native Messaging Integration Test
// Runs on a clean Windows machine after Inno Setup silent installation.
// Loads the real unpacked Radium extension in Chromium, sends native messaging
// RPC calls through Chrome to radium-nmh.exe, and verifies end-to-end IPC communication with radium.exe.

const { chromium } = require("playwright");
const fs = require("fs");
const path = require("path");
const { spawn, execSync } = require("child_process");

const ROOT = path.resolve(__dirname, "..", "..");
const EXTENSION_DIR = path.join(ROOT, "extension");
const EXPECTED_EXT_ID = "mdgmlcnpodoehidlplplfegngnjnilap";

function parseArgs() {
  const args = process.argv.slice(2);
  let installDir = null;
  let port = 0;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === "--install-dir" && i + 1 < args.length) {
      installDir = path.resolve(args[++i]);
    } else if (args[i] === "--port" && i + 1 < args.length) {
      port = parseInt(args[++i], 10);
    }
  }
  return { installDir, port };
}

async function runPositiveNativeMessagingTest() {
  console.log("=================================================================");
  console.log("  Radium Extension -- Native Messaging Positive Integration Test ");
  console.log("=================================================================");

  const { installDir, port } = parseArgs();
  if (!installDir || !fs.existsSync(installDir)) {
    throw new Error(`Invalid or missing --install-dir: ${installDir}`);
  }
  console.log(`Installed Application Root: ${installDir}`);

  // 1. Verify Native Messaging Manifest on Disk
  const manifestPath = path.join(installDir, "manifests", "com.radium.downloadmanager.chromium.json");
  if (!fs.existsSync(manifestPath)) {
    throw new Error(`Native Messaging manifest missing at: ${manifestPath}`);
  }
  const manifestData = JSON.parse(fs.readFileSync(manifestPath, "utf-8"));
  console.log("  [OK] Native Messaging manifest found:", manifestData.name);

  if (manifestData.name !== "com.radium.downloadmanager") {
    throw new Error(`Manifest name mismatch: "${manifestData.name}"`);
  }
  let expectedNmh = path.join(installDir, "radium-nmh.exe");
  if (!fs.existsSync(expectedNmh)) { expectedNmh = path.join(installDir, "flux-nmh.exe"); }
  if (path.resolve(manifestData.path) !== path.resolve(expectedNmh)) {
    throw new Error(`Manifest path "${manifestData.path}" != expected "${expectedNmh}"`);
  }
  const allowedOrigins = manifestData.allowed_origins || [];
  const expectedOrigin = `chrome-extension://${EXPECTED_EXT_ID}/`;
  if (!allowedOrigins.includes(expectedOrigin)) {
    throw new Error(`Manifest allowed_origins does not include "${expectedOrigin}": ${JSON.stringify(allowedOrigins)}`);
  }
  console.log(`  [OK] Manifest allowed_origins verified: ${expectedOrigin}`);

  // 2. Start Flux Desktop in Daemon Mode
  let fluxExe = path.join(installDir, "radium.exe");
  if (!fs.existsSync(fluxExe)) { fluxExe = path.join(installDir, "flux.exe"); }
  const testDb = path.join(installDir, "nm_test_state.db");
  const testDlDir = path.join(installDir, "nm_downloads");
  if (!fs.existsSync(testDlDir)) {
    fs.mkdirSync(testDlDir, { recursive: true });
  }

  console.log(`  Starting Radium daemon: ${fluxExe}...`);
  const daemonEnv = Object.assign({}, process.env, {
    RADIUM_ALLOW_LOOPBACK: "1",
    FLUX_ALLOW_LOOPBACK: "1",
    OPENIDM_ALLOW_LOOPBACK: "1"
  });

  const daemonProcess = spawn(fluxExe, [
    "--daemon",
    "--db", testDb,
    "--out", testDlDir,
    "--allow-loopback"
  ], {
    env: daemonEnv,
    stdio: "ignore",
    detached: true
  });
  daemonProcess.unref();

  // Give daemon 1.5s to initialize named pipe and IPC server
  await new Promise(r => setTimeout(r, 1500));

  // 3. Launch Chromium with Real Extension
  const tempProfile = fs.mkdtempSync(path.join(process.env.TEMP || "C:\\Temp", "pw-nm-"));
  let context = null;

  try {
    console.log("  Launching Chromium with unpacked Radium extension...");
    context = await chromium.launchPersistentContext(tempProfile, {
      headless: false,
      args: [
        `--disable-extensions-except=${EXTENSION_DIR}`,
        `--load-extension=${EXTENSION_DIR}`,
        "--no-sandbox",
        "--disable-gpu",
        "--disable-dev-shm-usage"
      ]
    });

    let [serviceWorker] = context.serviceWorkers();
    if (!serviceWorker) {
      serviceWorker = await context.waitForEvent("serviceworker", { timeout: 15000 });
    }
    console.log(`  Service Worker Active: ${serviceWorker.url()}`);

    // 4. Positive Native Messaging Exchange: system.handshake
    console.log("  [Step 1/3] Executing positive Native Messaging handshake (system.handshake)...");
    const handshakeResult = await serviceWorker.evaluate(async () => {
      return new Promise((resolve, reject) => {
        try {
          chrome.runtime.sendNativeMessage("com.radium.downloadmanager", {
            jsonrpc: "2.0",
            id: "test-hs-101",
            method: "system.handshake",
            params: { protocolVersion: "1.0" }
          }, (response) => {
            const err = chrome.runtime.lastError;
            if (err) {
              return reject(new Error(err.message));
            }
            resolve(response);
          });
        } catch (ex) {
          reject(ex);
        }
      });
    });

    console.log("  Handshake Response:", JSON.stringify(handshakeResult, null, 2));
    if (!handshakeResult || !handshakeResult.result) {
      throw new Error(`Native handshake failed or returned invalid RPC: ${JSON.stringify(handshakeResult)}`);
    }
    const res = handshakeResult.result;
    if (res.captureEnabled !== true) {
      throw new Error(`Expected captureEnabled === true, got: ${res.captureEnabled}`);
    }
    console.log(`  -> PASS: system.handshake succeeded! App version: ${res.appVersion}, Protocol: ${res.protocolVersion}`);

    // 5. Positive Native Messaging Exchange: download.offer
    console.log("  [Step 2/3] Submitting download offer via Native Messaging (download.offer)...");
    const targetUrl = (port > 0)
      ? `http://127.0.0.1:${port}/files/smoke_4mb.bin`
      : "http://127.0.0.1:8080/files/smoke_4mb.bin";

    const offerResult = await serviceWorker.evaluate(async (url) => {
      return new Promise((resolve, reject) => {
        try {
          chrome.runtime.sendNativeMessage("com.radium.downloadmanager", {
            jsonrpc: "2.0",
            id: "test-offer-102",
            method: "download.offer",
            params: {
              url,
              tabUrl: "http://127.0.0.1/downloads.html",
              filename: "nm_smoke_4mb.bin",
              totalBytes: 4194304
            }
          }, (response) => {
            const err = chrome.runtime.lastError;
            if (err) {
              return reject(new Error(err.message));
            }
            resolve(response);
          });
        } catch (ex) {
          reject(ex);
        }
      });
    }, targetUrl);

    console.log("  Download Offer Response:", JSON.stringify(offerResult, null, 2));
    if (!offerResult || !offerResult.result) {
      throw new Error(`Download offer RPC returned error: ${JSON.stringify(offerResult)}`);
    }
    if (offerResult.result.accepted !== true) {
      throw new Error(`Download offer was rejected by Radium: ${JSON.stringify(offerResult.result)}`);
    }
    const createdTaskId = offerResult.result.taskId;
    console.log(`  -> PASS: download.offer accepted! Created Task ID: ${createdTaskId}`);

    // 6. Verify Task Recorded in Radium Database
    console.log("  [Step 3/3] Verifying task registration in Radium task queue...");
    const listRes = execSync(`"${fluxExe}" --db "${testDb}" --list`, { encoding: "utf-8" });
    console.log(`  Radium Queue Output:\n${listRes.trim()}`);

    if (!listRes.includes("nm_smoke_4mb.bin") && !listRes.includes(createdTaskId)) {
      throw new Error(`Task ${createdTaskId} / nm_smoke_4mb.bin not found in Radium task list!`);
    }
    console.log("  -> PASS: Task registration verified in Radium persistent SQLite queue");

    console.log("\n=================================================================");
    console.log("   POSITIVE BROWSER NATIVE MESSAGING INTEGRATION TEST PASSED!    ");
    console.log("=================================================================");
    return 0;
  } finally {
    if (context) {
      await context.close().catch(() => {});
    }
    try {
      fs.rmSync(tempProfile, { recursive: true, force: true });
    } catch (_) {}

    // Gracefully stop the daemon and any subprocesses
    if (daemonProcess && daemonProcess.pid) {
      try {
        execSync(`taskkill /F /T /PID ${daemonProcess.pid}`, { stdio: "ignore" });
      } catch (_) {}
    }
    try {
      execSync(`taskkill /F /IM radium.exe`, { stdio: "ignore" });
    } catch (_) {}
    try {
      execSync(`taskkill /F /IM flux.exe`, { stdio: "ignore" });
    } catch (_) {}
  }
}

if (require.main === module) {
  runPositiveNativeMessagingTest()
    .then((code) => process.exit(code))
    .catch((err) => {
      console.error("\n[FAIL] Positive Native Messaging Integration Test Failed:\n", err);
      process.exit(1);
    });
}

module.exports = { runPositiveNativeMessagingTest };
