import { readFileSync } from 'node:fs'

const responsePrefix = 'juff-wake-'
const pcm = readFileSync(new URL('../assets/wake-ack.pcm', import.meta.url))
// Use the existing PCM playback path and keep individual transport chunks small.
const chunks = []
for (let offset = 0; offset < pcm.length; offset += 9600) {
  chunks.push(pcm.subarray(offset, offset + 9600).toString('base64'))
}

export function* wakeAcknowledgement(generation) {
  const responseId = `${responsePrefix}${generation}`
  for (const audio of chunks) {
    yield { type: 'audio.delta', responseId, sampleRate: 24000, audio,
      conversationGeneration: generation }
  }
  yield { type: 'audio.done', responseId, conversationGeneration: generation }
}

export function isWakeAcknowledgementPlayback(event) {
  return ['playback.started', 'playback.ended', 'playback.cancelled'].includes(event?.type)
    && typeof event.responseId === 'string' && event.responseId.startsWith(responsePrefix)
}
