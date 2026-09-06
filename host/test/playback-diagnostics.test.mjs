import assert from 'node:assert/strict'
import test from 'node:test'
import { PlaybackDiagnostics } from '../src/playback-diagnostics.mjs'

function fixture() {
  let now = 0
  const reports = []
  const diagnostics = new PlaybackDiagnostics({ now: () => now, report: event => reports.push(event) })
  return { diagnostics, reports, at: value => { now = value } }
}
const delta = (milliseconds, responseId = 'response') => ({ type: 'audio.delta', responseId,
  audio: Buffer.alloc(milliseconds * 48, 3).toString('base64') })
const credit = (received, underruns = 0, extra = {}) => ({ type: 'speaker.credit',
  responseId: 'response', received, underruns, dropped: 0, state: 'open', queuedMs: 0, ...extra })

test('underrun reports distinguish host backlog from an exhausted source without logging audio', () => {
  const { diagnostics: d, reports, at } = fixture()
  d.source(delta(400))
  d.device(credit(0, 20)) // Previous responses already underrun; do not blame this one.
  at(100)
  d.source(delta(3600))
  d.source({ type: 'audio.done', responseId: 'response' })
  at(500)
  d.device(credit(12000, 21))
  assert.deepEqual(reports[0], { type: 'underrun', sourceDone: true, sourceMs: 4000,
    receivedMs: 500, hostPendingMs: 3500, queuedMs: 0, sourceGapMs: 100,
    sourceAgeMs: 400, underruns: 1, dropped: 0 })
  d.device(credit(12000, 21))
  assert.equal(reports.length, 1, 'repeated credit does not repeat the warning')
  at(5000)
  d.device(credit(96000, 22, { state: 'ended' }))
  assert.equal(reports[1].hostPendingMs, 0)
  assert.equal(reports[1].underruns, 2)
  assert.equal(reports[1].type, 'ended')
  assert.equal(d.responses.size, 0)
  assert.ok(!JSON.stringify(reports).includes('responseId'))
  assert.ok(!JSON.stringify(reports).includes('audio'))
})

test('source stalls, terminal summaries and clear/reconnect boundaries remain separate', () => {
  const { diagnostics: d, reports, at } = fixture()
  d.source(delta(300))
  d.device(credit(0))
  at(1000)
  d.device(credit(7200, 1))
  assert.equal(reports[0].hostPendingMs, 0)
  assert.equal(reports[0].sourceDone, false)
  assert.equal(reports[0].sourceAgeMs, 1000)
  at(1500)
  d.source(delta(100))
  d.source({ type: 'audio.done', responseId: 'response' })
  d.device(credit(9600, 1, { state: 'ended' }))
  assert.equal(reports[1].sourceGapMs, 1500)
  assert.equal(reports[1].type, 'ended')
  d.source(delta(100))
  d.source({ type: 'playback.clear' })
  d.device(credit(0, 99, { state: 'cancelled' }))
  assert.equal(reports.length, 2)
  d.source(delta(100))
  d.clear()
  assert.equal(d.responses.size, 0)
})

test('malformed events and transcripts cannot enter bounded diagnostic state', () => {
  const { diagnostics: d, reports } = fixture()
  for (let index = 0; index < 100; index++) d.source(delta(10, `response-${index}`))
  assert.equal(d.responses.size, 32)
  d.source({ type: 'transcript.final', content: 'private-content' })
  d.source(delta(10))
  d.device(credit(-1, 1, { state: 'ended' }))
  d.device(credit(1, NaN, { state: 'ended' }))
  assert.equal(reports.length, 0)
  assert.ok(!JSON.stringify([...d.responses.values()]).includes('private-content'))
  assert.ok(!JSON.stringify([...d.responses.values()]).includes('audio'))
})
