<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
})
const config = ref(props.config)
</script>

<template>
  <!--webrtc_enabled-->
  <div class="mb-3">
    <Checkbox
      id="webrtc_enabled"
      locale-prefix="config"
      v-model="config.webrtc_enabled"
      default="false"
    ></Checkbox>

    <!--webrtc_stun_server-->
    <div v-if="config.webrtc_enabled === 'enabled'" class="mt-3">
      <label for="webrtc_stun_server" class="form-label">{{ $t("config.webrtc_stun_server") }}</label>
      <input
        type="text"
        class="form-control"
        id="webrtc_stun_server"
        placeholder="stun:stun.example.com:19302"
        v-model="config.webrtc_stun_server"
      />
      <div class="form-text">{{ $t("config.webrtc_stun_server_desc") }}</div>
    </div>
  </div>
</template>
