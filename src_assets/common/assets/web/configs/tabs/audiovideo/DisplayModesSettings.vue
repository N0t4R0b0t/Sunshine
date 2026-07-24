<script setup>
import { ref, computed } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps([
  'platform',
  'config',
])
const config = ref(props.config)

// force_video_output_fps is a single int on the backend (0 = disabled), but
// the UI presents it as a checkbox + a number field that only appears when
// checked - this bridges the two without adding a second config key.
const forceVideoOutputFpsEnabled = computed({
  get: () => (Number(config.value.force_video_output_fps) > 0 ? 'enabled' : 'disabled'),
  set: (value) => {
    config.value.force_video_output_fps = value === 'enabled' ? 30 : 0
  },
})
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="form-control" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>

  <!--force_video_output_fps-->
  <div class="mb-3">
    <Checkbox
      id="force_video_output_fps_enabled"
      locale-prefix="config"
      v-model="forceVideoOutputFpsEnabled"
      default="false"
    ></Checkbox>

    <div v-if="config.force_video_output_fps > 0" class="mt-2">
      <label for="force_video_output_fps" class="form-label">{{ $t("config.force_video_output_fps") }}</label>
      <input type="number" min="1" max="1000" class="form-control" id="force_video_output_fps" placeholder="30" v-model="config.force_video_output_fps" />
      <div class="form-text">{{ $t("config.force_video_output_fps_desc") }}</div>
    </div>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
