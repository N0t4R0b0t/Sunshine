<script setup>
import { ref, onMounted } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'
import { apiFetch } from '../../../fetch_utils'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)

const liveSinks = ref([])
const liveSinksSupported = ref(false)

onMounted(async () => {
  if (props.platform !== 'linux') {
    return
  }

  try {
    const response = await apiFetch('./api/audio-sinks')
    if (!response.ok) {
      return
    }
    const data = await response.json()
    liveSinksSupported.value = !!data.supported
    liveSinks.value = data.sinks || []
  } catch (e) {
    console.debug('AudioSinkSelector: failed to fetch live audio sinks', e)
  }
})
</script>

<template>
  <div class="mb-3">
    <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
    <select v-if="platform === 'linux' && liveSinksSupported" class="form-select" id="audio_sink"
            v-model="config.audio_sink">
      <option value="">{{ $t('config.audio_sink_auto') }}</option>
      <option v-for="sink in liveSinks" :key="sink.id" :value="sink.id">
        {{ sink.friendly_name }}
      </option>
    </select>
    <input v-else type="text" class="form-control" id="audio_sink"
           :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
           v-model="config.audio_sink" />
    <div class="form-text">
      {{ $tp('config.audio_sink_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre>tools\audio-info.exe</pre>
        </template>
        <template #freebsd>
          <pre>pacmd list-sinks | grep "name:"</pre>
          <pre>pactl info | grep Source</pre>
        </template>
        <template #linux>
          <pre>pacmd list-sinks | grep "name:"</pre>
          <pre>pactl info | grep Source</pre>
        </template>
        <template #macos>
          <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
          <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
