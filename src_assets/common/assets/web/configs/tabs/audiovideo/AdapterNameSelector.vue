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
const adapterNamePlaceholder = '/dev/dri/renderD128'

const liveAdapters = ref([])
const liveAdaptersSupported = ref(false)

onMounted(async () => {
  if (props.platform !== 'linux') {
    return
  }

  try {
    const response = await apiFetch('./api/adapters')
    if (!response.ok) {
      return
    }
    const data = await response.json()
    liveAdaptersSupported.value = !!data.supported
    liveAdapters.value = data.adapters || []
  } catch (e) {
    console.debug('AdapterNameSelector: failed to fetch live adapters', e)
  }
})
</script>

<template>
  <div class="mb-3" v-if="platform !== 'macos'">
    <label for="adapter_name" class="form-label">{{ $t('config.adapter_name') }}</label>
    <select v-if="platform === 'linux' && liveAdaptersSupported" class="form-select" id="adapter_name"
            v-model="config.adapter_name">
      <option value="">{{ $t('config.adapter_name_auto') }}</option>
      <option v-for="adapter in liveAdapters" :key="adapter.id" :value="adapter.id">
        {{ adapter.friendly_name }}
      </option>
    </select>
    <input v-else type="text" class="form-control" id="adapter_name"
           :placeholder="$tp('config.adapter_name_placeholder', adapterNamePlaceholder)"
           v-model="config.adapter_name" />
    <div class="form-text">
      <PlatformLayout :platform="platform">
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}<br>
          <pre>tools\dxgi-info.exe</pre>
        </template>
        <template #freebsd>
          {{ $t('config.adapter_name_desc_linux_1') }}<br>
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre>
          {{ $t('config.adapter_name_desc_linux_3') }}<br>
          <i>VAProfileH264High   : VAEntrypointEncSlice</i>
        </template>
        <template #linux>
          {{ $t('config.adapter_name_desc_linux_1') }}<br>
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre>
          {{ $t('config.adapter_name_desc_linux_3') }}<br>
          <i>VAProfileH264High   : VAEntrypointEncSlice</i>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
