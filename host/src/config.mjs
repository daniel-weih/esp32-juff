import process from 'node:process'

function integer(value, fallback, name) {
  const parsed = Number.parseInt(value ?? '', 10)
  if (Number.isInteger(parsed) && parsed >= 0 && parsed <= 65535) return parsed
  if (value === undefined || value === '') return fallback
  throw new Error(`${name} must be an integer between 0 and 65535`)
}

function boolean(value, fallback = false) {
  if (value === undefined || value === '') return fallback
  return ['1', 'true', 'yes', 'on'].includes(String(value).toLowerCase())
}

function httpUrl(value, name) {
  let url
  try {
    url = new URL(value)
  } catch {
    throw new Error(`${name} must be a valid URL`)
  }
  if (!['http:', 'https:'].includes(url.protocol)) {
    throw new Error(`${name} must use http or https`)
  }
  return url.origin
}

export function readConfig(env = process.env) {
  const deviceToken = String(env.JUFF_DEVICE_TOKEN || '')
  if (
    deviceToken.length < 16
    || /^(change|replace)[-_]/i.test(deviceToken)
  ) {
    throw new Error('JUFF_DEVICE_TOKEN must be a private random value of at least 16 characters')
  }

  const devicePath = String(env.JUFF_DEVICE_PATH || '/device')
  if (!devicePath.startsWith('/')) {
    throw new Error('JUFF_DEVICE_PATH must start with /')
  }

  return {
    bridgeHost: String(env.JUFF_BRIDGE_HOST || '127.0.0.1'),
    bridgePort: integer(env.JUFF_BRIDGE_PORT, 8765, 'JUFF_BRIDGE_PORT'),
    devicePath,
    deviceToken,
    gatewayUrl: httpUrl(env.QWEN_GATEWAY_URL || 'http://127.0.0.1:3101', 'QWEN_GATEWAY_URL'),
    gatewaySessionId: String(env.QWEN_SESSION_ID || 'esp32-main'),
    gatewayTakeover: boolean(env.QWEN_TAKEOVER),
    workingDirectory: String(env.QWEN_WORKING_DIRECTORY || process.cwd()),
  }
}
