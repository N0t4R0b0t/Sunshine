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
let _outputNamePlaceholder = '0';
if(props.platform === 'windows') {
  _outputNamePlaceholder = '{de9bb7e2-186e-505b-9e93-f48793333810}';
} else if(props.platform === 'linux' || props.platform === 'freebsd') {
  _outputNamePlaceholder = 'DP-0';
}
const outputNamePlaceholder = _outputNamePlaceholder;  // NOSONAR(javascript:S1481,javascript:S1854): Constant used by vue.js binding for placeholder below

const liveOutputs = ref([])
const liveOutputsSupported = ref(false)

onMounted(async () => {
  if (props.platform !== 'linux') {
    return
  }

  try {
    const response = await apiFetch('./api/display/outputs')
    if (!response.ok) {
      return
    }
    const data = await response.json()
    liveOutputsSupported.value = !!data.supported
    liveOutputs.value = data.outputs || []
  } catch (e) {
    console.debug('DisplayOutputSelector: failed to fetch live outputs', e)
  }
})
</script>

<template>
  <div class="mb-3">
    <label for="output_name" class="form-label">{{ $t('config.output_name') }}</label>
    <select v-if="platform === 'linux' && liveOutputsSupported" class="form-select" id="output_name"
            v-model="config.output_name">
      <option value="">{{ $t('config.output_name_auto') }}</option>
      <option v-for="output in liveOutputs" :key="output.id" :value="output.id">
        {{ output.friendly_name }} ({{ output.width }}x{{ output.height }}{{ output.primary ? ', primary' : '' }})
      </option>
    </select>
    <input v-else type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
           v-model="config.output_name"/>
    <div class="form-text">
      {{ $tp('config.output_name_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre style="white-space: pre-line;">
            <b>&nbsp;&nbsp;{</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
            <b>&nbsp;&nbsp;}</b>
          </pre>
        </template>
        <template #freebsd>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: HDMI-A-1 connected: true
            Info: Detected display: DP-1 connected: true
            Info: Detected display: DP-2 connected: false
            Info: Detected display: DVI-D-3 connected: false
          </pre>
        </template>
        <template #linux>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: HDMI-A-1 connected: true
            Info: Detected display: DP-1 connected: true
            Info: Detected display: DP-2 connected: false
            Info: Detected display: DVI-D-3 connected: false
          </pre>
        </template>
        <template #macos>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
          </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
