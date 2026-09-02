import assert from 'node:assert/strict'
import test from 'node:test'
import { readConfig } from '../src/config.mjs'

function environment(token) {
  return {
    JUFF_DEVICE_TOKEN: token,
    QWEN_GATEWAY_URL: 'http://127.0.0.1:3101',
  }
}

test('accepts a private device token', () => {
  const config = readConfig(environment('7f5e24a341b84dd78b5c6f48'))
  assert.equal(config.bridgeHost, '127.0.0.1')
  assert.equal(config.bridgePort, 8765)
})

test('rejects short and placeholder device tokens', () => {
  assert.throws(() => readConfig(environment('too-short')), /private random value/)
  assert.throws(
    () => readConfig(environment('replace-with-a-random-device-token')),
    /private random value/,
  )
})
