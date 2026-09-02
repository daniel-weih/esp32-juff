import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import process from 'node:process'
import { readConfig } from './config.mjs'
import { createDeviceBridge } from './device-bridge.mjs'
import { GatewayClient } from './gateway-client.mjs'

const hostDirectory = fileURLToPath(new URL('..', import.meta.url))
const qwenExecutable = fileURLToPath(new URL('../node_modules/.bin/qwenaudio', import.meta.url))
const bleExecutable = fileURLToPath(
  new URL('../../macos/build/JuffBLE.app/Contents/MacOS/JuffBLE', import.meta.url),
)
const config = readConfig()

let bridge = null
let gateway = null
let bleCompanion = null
let bleGateway = null
let bleGatewayStarting = null
let stopping = false
let bleMicWindowStartedAt = Date.now()
let bleMicFrames = 0
let bleMicBytes = 0
let bleMicPeakRms = 0

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds))
}

async function gatewayHealth() {
  try {
    const response = await fetch(`${config.gatewayUrl}/api/health`, {
      signal: AbortSignal.timeout(1000),
    })
    const health = await response.json().catch(() => null)
    return response.ok && health?.backend ? health : null
  } catch {
    return null
  }
}

async function waitForGateway(timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    const health = await gatewayHealth()
    if (health) return health
    if (gateway?.exitCode !== null) {
      throw new Error(`Qwen Gateway exited before becoming ready (code ${gateway.exitCode})`)
    }
    await delay(200)
  }
  throw new Error(`Qwen Gateway did not become ready at ${config.gatewayUrl}`)
}

async function stop(signal) {
  if (stopping) return
  stopping = true
  process.stdout.write(`Stopping Juff host services (${signal})…\n`)
  if (bridge) {
    await bridge.close()
    bridge = null
  }
  bleGateway?.close()
  bleGateway = null
  if (bleCompanion?.exitCode === null) bleCompanion.kill('SIGTERM')
  bleCompanion = null
  if (gateway?.exitCode === null) gateway.kill('SIGTERM')
}

const BLE_DEVICE_EVENTS = new Set([
  'interrupt',
  'input.suspend.ack',
  'playback.started',
  'playback.ended',
  'playback.cancelled',
])

const BLE_GATEWAY_EVENTS = new Set([
  'device.ready',
  'voice.ready',
  'voice.deactivated',
  'audio.begin',
  'audio.delta',
  'audio.done',
  'playback.clear',
  'input.suspend',
  'input.resume',
  'voice.state',
  'error',
])

function sendToBle(message) {
  if (!bleCompanion?.stdin?.writable) return false
  return bleCompanion.stdin.write(`${JSON.stringify(message)}\n`)
}

function closeBleVoice() {
  const client = bleGateway
  bleGateway = null
  client?.close()
}

async function startBleVoice() {
  if (stopping || bleGateway?.connected) return
  if (bleGatewayStarting) return bleGatewayStarting

  bleGatewayStarting = (async () => {
    const client = new GatewayClient({
      baseUrl: config.gatewayUrl,
      sessionId: `${config.gatewaySessionId}-ble`,
      takeover: true,
      inputEnabled: true,
      outputEnabled: true,
      workingDirectory: config.workingDirectory,
    })
    bleGateway = client
    let activeResponseId = ''
    client.on('event', event => {
      if (event?.type === 'transcript.final') {
        process.stdout.write(
          `QWEN ${event.role || 'voice'}: ${event.content || event.text || ''}\n`,
        )
      }
      if (
        event?.type === 'error'
        || event?.type === 'response.started'
        || event?.type === 'response.done'
        || event?.type === 'audio.begin'
        || event?.type === 'audio.done'
        || event?.type === 'playback.clear'
      ) {
        const detail = event.message || event.error?.message || event.responseId || ''
        process.stdout.write(`QWEN EVENT ${event.type}${detail ? `: ${detail}` : ''}\n`)
      }
      if (event?.type === 'audio.delta') {
        const responseId = String(event.responseId || '')
        if (responseId && activeResponseId !== responseId) {
          activeResponseId = responseId
          sendToBle({
            type: 'audio.begin',
            responseId,
            sampleRate: Number(event.sampleRate) || 24000,
          })
          process.stdout.write(`QWEN EVENT audio.begin: ${responseId}\n`)
        }
      } else if (event?.type === 'playback.clear') {
        activeResponseId = ''
      }
      if (BLE_GATEWAY_EVENTS.has(event?.type)) sendToBle(event)
    })
    client.on('error', error => {
      process.stderr.write(`BLE Qwen websocket error: ${error.message}\n`)
    })
    client.on('close', (code, reason) => {
      if (bleGateway === client) {
        bleGateway = null
        sendToBle({ type: 'voice.deactivated' })
        if (!stopping) {
          process.stderr.write(`BLE Qwen session closed (${code} ${reason})\n`)
        }
      }
    })

    try {
      const health = await client.connect()
      sendToBle({
        type: 'device.ready',
        realtimeModel: health.realtimeModel || health.realtimeModelProfile?.id || '',
        inputSampleRate: Number(health.realtimeInputSampleRate) || 16000,
        outputSampleRate: 24000,
        transport: 'ble',
        transportCodec: 'g711-mulaw',
      })
      process.stdout.write(
        `BLE voice ready: ${health.realtimeModel || 'Qwen Realtime'} via ESP32 microphone/speaker\n`,
      )
    } catch (error) {
      if (bleGateway === client) bleGateway = null
      client.close()
      sendToBle({ type: 'error', message: error.message })
      process.stderr.write(`Unable to start BLE Qwen session: ${error.message}\n`)
    }
  })().finally(() => {
    bleGatewayStarting = null
  })
  return bleGatewayStarting
}

function handleBleLine(line) {
  if (line.startsWith('MIC ')) {
    const encoded = line.slice(4)
    if (bleGateway?.connected && encoded) {
      const pcm = Buffer.from(encoded, 'base64')
      bleGateway.sendAudio(pcm)
      let energy = 0
      for (let offset = 0; offset + 1 < pcm.length; offset += 2) {
        const sample = pcm.readInt16LE(offset)
        energy += sample * sample
      }
      const sampleCount = Math.floor(pcm.length / 2)
      const rms = sampleCount ? Math.sqrt(energy / sampleCount) : 0
      bleMicFrames += 1
      bleMicBytes += pcm.length
      bleMicPeakRms = Math.max(bleMicPeakRms, rms)
      const now = Date.now()
      const elapsed = now - bleMicWindowStartedAt
      if (elapsed >= 1000) {
        process.stdout.write(
          `BLE MIC ${(bleMicFrames * 1000 / elapsed).toFixed(1)} fps; `
            + `${(bleMicBytes * 1000 / elapsed / 1024).toFixed(1)} KiB/s; `
            + `peak RMS ${Math.round(bleMicPeakRms)}\n`,
        )
        bleMicWindowStartedAt = now
        bleMicFrames = 0
        bleMicBytes = 0
        bleMicPeakRms = 0
      }
    }
    return
  }
  if (line.startsWith('DEVICE ')) {
    process.stdout.write(`BLE ${line}\n`)
    try {
      const event = JSON.parse(line.slice(7))
      if (BLE_DEVICE_EVENTS.has(event?.type)) bleGateway?.send(event)
    } catch {
      // Human-readable device output can be ignored when it is not JSON.
    }
    return
  }
  process.stdout.write(`BLE ${line}\n`)
  if (line.startsWith('STATE audio-connected:')) {
    void startBleVoice()
  } else if (line.startsWith('STATE disconnected')) {
    closeBleVoice()
  }
}

function relayBleOutput(stream, onLine) {
  let buffer = ''
  stream.setEncoding('utf8')
  stream.on('data', chunk => {
    buffer += chunk
    while (buffer.includes('\n')) {
      const newline = buffer.indexOf('\n')
      const line = buffer.slice(0, newline).trim()
      buffer = buffer.slice(newline + 1)
      if (line) onLine(line)
    }
  })
}

function startBleCompanion() {
  if (!existsSync(bleExecutable)) {
    process.stdout.write('Bluetooth companion not built; run npm run ble:build\n')
    return
  }
  bleCompanion = spawn(bleExecutable, [], {
    cwd: hostDirectory,
    stdio: ['pipe', 'pipe', 'pipe'],
  })
  relayBleOutput(bleCompanion.stdout, handleBleLine)
  relayBleOutput(bleCompanion.stderr, line => process.stderr.write(`BLE ${line}\n`))
  bleCompanion.once('error', error => {
    process.stderr.write(`Unable to start Bluetooth companion: ${error.message}\n`)
  })
  bleCompanion.once('exit', (code, signal) => {
    closeBleVoice()
    if (!stopping) {
      process.stderr.write(
        `Bluetooth companion stopped (code=${code}, signal=${signal})\n`,
      )
    }
    bleCompanion = null
  })
}

process.once('SIGINT', () => void stop('SIGINT'))
process.once('SIGTERM', () => void stop('SIGTERM'))

try {
  let health = await gatewayHealth()
  if (!health) {
    gateway = spawn(qwenExecutable, [], {
      cwd: hostDirectory,
      stdio: 'inherit',
    })
    gateway.once('error', error => {
      process.stderr.write(`Unable to start Qwen Gateway: ${error.message}\n`)
    })
    gateway.once('exit', (code, signal) => {
      if (!stopping && bridge) {
        process.stderr.write(`Qwen Gateway stopped unexpectedly (code=${code}, signal=${signal})\n`)
        process.exitCode = code || 1
        void stop('gateway exit')
      }
    })
    health = await waitForGateway()
  }

  startBleCompanion()
  bridge = createDeviceBridge(config)
  await bridge.start()
  process.stdout.write(
    `Juff ready: ${health.realtimeModel || 'Qwen Realtime'}; `
      + `ESP32 bridge ws://${config.bridgeHost}:${config.bridgePort}${config.devicePath}\n`,
  )
} catch (error) {
  process.stderr.write(`Juff startup failed: ${error.message}\n`)
  process.exitCode = 1
  await stop('startup failure')
}
