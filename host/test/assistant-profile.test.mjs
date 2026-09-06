import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { mkdtemp, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { AssistantProfileStore, resolveAssistantFile, XIAOZHI_PROFILE } from '../src/manager/assistant.mjs'

async function fixture(t) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'juff-assistant-'))
  t.after(() => rm(directory, { recursive: true, force: true }))
  const store = new AssistantProfileStore({ file: path.join(directory, 'private/ASSISTANT.md') })
  return { directory, store }
}

test('profile reads are side-effect free; saves are private, durable and retain the previous version', async t => {
  const { directory, store } = await fixture(t)
  const original = await store.read()
  assert.equal(original.content, '')
  assert.deepEqual(await readdir(directory), [])
  const first = await store.save({ content: '你叫测试伙伴。', revision: original.revision })
  const second = await store.save({ content: XIAOZHI_PROFILE, revision: first.revision })
  assert.equal((await store.read()).revision, second.revision)
  assert.equal(await readFile(store.file + '.previous', 'utf8'), first.content)
  await store.save({ content: second.content, revision: second.revision })
  assert.equal(await readFile(store.file + '.previous', 'utf8'), first.content, 'reapplying unchanged settings preserves the backup')
  for (const file of [store.file, store.file + '.previous']) assert.equal((await stat(file)).mode & 0o777, 0o600)
  assert.equal((await stat(path.dirname(store.file))).mode & 0o777, 0o700)
  assert.deepEqual((await readdir(path.dirname(store.file))).sort(), ['ASSISTANT.md', 'ASSISTANT.md.previous'])
})

test('invalid edits and stale browser revisions preserve the saved profile', async t => {
  const { store } = await fixture(t)
  const initial = await store.read()
  for (const content of ['', '   ', 'a\0b', 'x'.repeat(4001), '😀'.repeat(4001), 42]) {
    await assert.rejects(store.save({ content, revision: initial.revision }), /1–4000/)
  }
  await assert.rejects(store.save({ content: '新设定', revision: initial.revision, path: '/tmp/arbitrary' }), /1–4000/)
  await assert.rejects(store.save({ content: '新设定', revision: initial.revision, reconnect: 'yes' }), /重连/)
  const first = await store.save({ content: '😀'.repeat(4000), revision: initial.revision })
  assert.equal([...first.content].length, 4000)
  await assert.rejects(store.save({ content: '来自过期页面', revision: initial.revision }), /其他页面/)
  await writeFile(store.file, '在文件中修改')
  await assert.rejects(store.save({ content: '再次过期', revision: first.revision }), /其他页面/)
  assert.equal((await store.read()).content, '在文件中修改')
})

test('path resolution follows installed Qwen config/env precedence without initializing user data', async t => {
  const { directory } = await fixture(t)
  const runtimeRoot = path.join(directory, 'runtime'), data = path.join(directory, 'data')
  await mkdir(runtimeRoot); await mkdir(data)
  const env = { QWAUDIO_CONFIG_DIR: path.join(directory, 'runtime-state'), QWAUDIO_DATA_DIR: data,
    QWEN_AUDIO_AGENT_RUNTIME_ROOT: runtimeRoot }
  assert.equal(await resolveAssistantFile(env), path.join(data, 'ASSISTANT.md'))
  await writeFile(path.join(data, 'config.env'), 'QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH=custom/persona.md\nDASHSCOPE_API_KEY=test-private-value\n')
  assert.equal(await resolveAssistantFile(env), path.join(runtimeRoot, 'custom/persona.md'))
  assert.equal(await resolveAssistantFile({ ...env, QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH: '/tmp/explicit-persona.md' }), '/tmp/explicit-persona.md')
  assert.equal(env.DASHSCOPE_API_KEY, undefined)
  assert.deepEqual((await readdir(directory)).sort(), ['data', 'runtime'])
  assert.deepEqual(await readdir(data), ['config.env'])
  assert.equal(await resolveAssistantFile({ XDG_CONFIG_HOME: directory, QWEN_AUDIO_AGENT_RUNTIME_ROOT: runtimeRoot }), path.join(directory, 'qwaudio/ASSISTANT.md'))
})

test('the installed Qwen voice instructions consume and reload the saved profile, keeping core rules', async t => {
  const { store, directory } = await fixture(t)
  await store.save({ content: XIAOZHI_PROFILE, revision: (await store.read()).revision })
  const moduleURL = new URL('../node_modules/qwen-audio-agent/server/src/voice/frontend-tools.mjs', import.meta.url)
  const script = `import assert from 'node:assert/strict';
    import { writeFileSync } from 'node:fs';
    import { buildFrontendInstructions } from ${JSON.stringify(moduleURL.href)};
    const first = buildFrontendInstructions();
    assert.ok(first.includes('默认昵称叫“小智”'));
    assert.ok(first.includes('# Permission requests'));
    assert.ok(first.includes('<assistant_profile authority="persona_only">'));
    writeFileSync(process.env.QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH, '默认昵称叫“小舟”。');
    const next = buildFrontendInstructions();
    assert.ok(next.includes('默认昵称叫“小舟”'));
    assert.ok(!next.includes('默认昵称叫“小智”'));
    console.log('installed Qwen profile contract OK');`
  const output = execFileSync(process.execPath, ['--input-type=module', '-e', script], {
    cwd: fileURLToPath(new URL('..', import.meta.url)), encoding: 'utf8',
    env: { PATH: process.env.PATH, QWAUDIO_CONFIG_DIR: path.join(directory, 'qwen'),
      QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH: store.file, AGENT_PROTOCOL: 'none' },
  })
  assert.match(output, /profile contract OK/)
})
