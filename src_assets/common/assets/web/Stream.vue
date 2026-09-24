<template>
  <Navbar v-if="phase === 'setup'"></Navbar>
  <div id="content" :class="phase === 'setup' ? 'container-fluid' : 'stream-fullpage'">
    <div v-if="!checkedConfig" class="container my-4">
      {{ $t('_common.loading') }}
    </div>

    <!-- Feature disabled -->
    <div v-else-if="!webrtcEnabled" class="container my-4">
      <h1>{{ $t('stream.title') }}</h1>
      <div class="alert alert-warning my-4">
        <alert-triangle :size="18" class="icon"></alert-triangle>
        {{ $t('stream.disabled_notice') }}
        <RouterLink to="/config#av">{{ $t('stream.disabled_notice_link') }}</RouterLink>
      </div>
    </div>

    <!-- Setup form -->
    <div v-else-if="phase === 'setup'" class="container my-4">
      <h1>{{ $t('stream.title') }}</h1>
      <p>{{ $t('stream.description') }}</p>

      <div v-if="!isSecureContext" class="alert alert-warning my-3">
        <alert-triangle :size="18" class="icon"></alert-triangle>
        {{ $t('stream.insecure_context_notice') }}
      </div>

      <div class="card my-3">
        <div class="card-body">
          <div class="mb-3">
            <label for="streamApp" class="form-label">{{ $t('stream.application') }}</label>
            <select id="streamApp" class="form-select" v-model.number="appid">
              <option :value="0">{{ $t('stream.desktop') }}</option>
              <option v-for="app in apps" :key="app.index" :value="app.index">{{ app.name }}</option>
            </select>
          </div>

          <div class="row">
            <div class="col-md-4 mb-3">
              <label for="streamResolution" class="form-label">{{ $t('stream.resolution') }}</label>
              <select id="streamResolution" class="form-select" v-model="resolutionKey">
                <option v-for="preset in resolutionPresets" :key="preset.key" :value="preset.key">{{ preset.label }}</option>
              </select>
            </div>
            <div class="col-md-4 mb-3">
              <label for="streamFps" class="form-label">{{ $t('stream.fps') }}</label>
              <select id="streamFps" class="form-select" v-model.number="fps">
                <option v-for="rate in [30, 60]" :key="rate" :value="rate">{{ rate }}</option>
              </select>
            </div>
            <div class="col-md-4 mb-3">
              <label for="streamBitrate" class="form-label">{{ $t('stream.bitrate') }} ({{ bitrate }} Kbps)</label>
              <input id="streamBitrate" type="range" class="form-range" min="2000" max="50000" step="1000" v-model.number="bitrate" />
            </div>
          </div>
        </div>
      </div>

      <div class="alert alert-danger my-3" v-if="error">
        <b>{{ $t('_common.error') }}</b> {{ error }}
      </div>

      <button type="button" class="btn btn-primary" :disabled="phase === 'connecting'" @click="connect">
        <play :size="18" class="icon"></play>
        {{ phase === 'connecting' ? $t('stream.connecting') : $t('stream.connect') }}
      </button>
    </div>

    <!-- Connecting / connected -->
    <div v-else class="stream-view" :class="{ 'stream-hide-cursor': controlling && !mouseLocked }">
      <video ref="video" class="stream-video" autoplay playsinline></video>

      <div
        v-if="localCursor"
        class="local-cursor"
        :style="{ left: localCursor.clientX + 'px', top: localCursor.clientY + 'px' }"
      >
        <mouse-pointer-2 :size="20"></mouse-pointer-2>
      </div>

      <div v-if="phase === 'connecting'" class="stream-overlay">
        <div class="spinner-border text-light mb-3" role="status"></div>
        <div>{{ $t('stream.connecting') }}</div>
      </div>

      <div v-else-if="!controlling" class="stream-overlay stream-overlay-click" @click="startControlling">
        <mouse-pointer-click :size="48" class="mb-3"></mouse-pointer-click>
        <div>{{ $t('stream.click_to_control') }}</div>
        <div class="stream-shortcuts-hint">{{ $t('stream.shortcuts_hint') }}</div>
      </div>

      <div v-if="phase === 'connected'" class="stream-toolbar">
        <button
          v-if="controlling"
          type="button"
          class="btn btn-sm"
          :class="mouseLocked ? 'btn-primary' : 'btn-secondary'"
          @click="toggleMouseLock"
          :title="mouseLocked ? $t('stream.unlock_mouse') : $t('stream.lock_mouse')"
        >
          <crosshair :size="16"></crosshair>
        </button>
        <button type="button" class="btn btn-sm btn-secondary" @click="toggleFullscreen" :title="$t('stream.fullscreen')">
          <maximize :size="16"></maximize>
        </button>
        <button type="button" class="btn btn-sm btn-secondary" @click="toggleStats" :title="$t('stream.stats')">
          <activity :size="16"></activity>
        </button>
        <button type="button" class="btn btn-sm btn-danger" @click="disconnect" :title="$t('stream.disconnect')">
          <square :size="16"></square>
        </button>
      </div>

      <div v-if="showStats && phase === 'connected'" class="stream-stats">
        <div>{{ $t('stream.stats_resolution') }}: {{ stats.video?.frameWidth || 0 }}x{{ stats.video?.frameHeight || 0 }}</div>
        <div>{{ $t('stream.stats_decoded') }}: {{ stats.video?.framesDecoded || 0 }} ({{ $t('stream.stats_dropped') }} {{ stats.video?.framesDropped || 0 }})</div>
        <div>{{ $t('stream.stats_loss') }}: {{ packetLossPercent }}%</div>
        <div>{{ $t('stream.stats_rtt') }}: {{ stats.roundTripMs != null ? Math.round(stats.roundTripMs) + ' ms' : '-' }}</div>
        <div>{{ $t('stream.stats_candidate') }}: {{ stats.candidateType || '-' }}</div>
        <div>{{ $t('stream.stats_audio') }}: {{ stats.audio ? stats.audio.codec : $t('stream.stats_no_audio') }}</div>
      </div>

      <div v-if="phase === 'failed'" class="stream-overlay">
        <alert-triangle :size="48" class="mb-3"></alert-triangle>
        <div class="mb-3">{{ error || $t('stream.connection_lost') }}</div>
        <button type="button" class="btn btn-primary me-2" @click="connect">
          <play :size="18" class="icon"></play>
          {{ $t('stream.reconnect') }}
        </button>
        <button type="button" class="btn btn-secondary" @click="reset">
          {{ $t('_common.cancel') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script>
import { markRaw } from 'vue'
import { Activity, AlertTriangle, Crosshair, Maximize, MousePointer2, MousePointerClick, Play, Square } from '@lucide/vue'
import Navbar from './Navbar.vue'
import { apiFetch } from './fetch_utils'
import { StreamConnection } from './stream/connection.js'
import { InputSender } from './stream/input.js'

/** Common resolutions offered in the setup form. */
const RESOLUTION_PRESETS = [
  { key: '1280x720', label: '1280x720', width: 1280, height: 720 },
  { key: '1920x1080', label: '1920x1080', width: 1920, height: 1080 },
  { key: '2560x1440', label: '2560x1440', width: 2560, height: 1440 },
  { key: '3840x2160', label: '3840x2160 (4K)', width: 3840, height: 2160 },
]

/** How often the stats overlay refreshes while visible. */
const STATS_INTERVAL_MS = 1000

/**
 * Local page shortcuts, keyed by `KeyboardEvent.code`, all held with Ctrl+Alt+Shift together as
 * Moonlight's own desktop clients use. These are intercepted here and never forwarded to the host as
 * keystrokes. Moonlight also binds M (toggle the host cursor's visibility) and N (minimize); neither
 * has a browser equivalent - there is no way for a page to hide only the video's cursor rendering
 * decision made by the host, and browsers deliberately do not let a page minimize its own window.
 */
const SHORTCUT_KEYS = {
  KeyQ: 'disconnect',
  KeyZ: 'toggleMouseLock',
  KeyX: 'toggleFullscreen',
  KeyS: 'toggleStats',
}

export default {
  components: {
    Navbar,
    Play,
    Square,
    Maximize,
    Activity,
    AlertTriangle,
    Crosshair,
    MousePointer2,
    MousePointerClick,
  },
  data() {
    return {
      checkedConfig: false,
      webrtcEnabled: false,
      apps: [],

      appid: 0,
      resolutionKey: '1920x1080',
      fps: 60,
      bitrate: 20000,
      resolutionPresets: RESOLUTION_PRESETS,

      /** @type {'setup'|'connecting'|'connected'|'failed'} */
      phase: 'setup',
      error: null,
      controlling: false,
      mouseLocked: false,
      isFullscreen: false,
      showStats: false,
      stats: {},
      /** @type {{clientX: number, clientY: number}|null} Where to draw the local cursor, in viewport pixels. */
      localCursor: null,
      isSecureContext: typeof window !== 'undefined' ? window.isSecureContext : true,

      connection: null,
      input: null,
      statsTimer: null,
      stream: null,
    }
  },
  computed: {
    packetLossPercent() {
      const video = this.stats.video
      if (!video) {
        return '0.0'
      }
      const total = video.packetsReceived + video.packetsLost
      return total > 0 ? ((100 * video.packetsLost) / total).toFixed(1) : '0.0'
    },
  },
  async mounted() {
    document.addEventListener('fullscreenchange', this.onFullscreenChange)
    document.addEventListener('pointerlockchange', this.onPointerLockChange)
    window.addEventListener('pagehide', this.disconnect)
    // Registered before startControlling() ever attaches InputSender's own keydown listener, so
    // stopImmediatePropagation() below reliably keeps a matched shortcut from also being forwarded
    // to the host as a keystroke.
    window.addEventListener('keydown', this.onShortcutKeydown)

    try {
      const [configResponse, appsResponse] = await Promise.all([apiFetch('/api/config'), apiFetch('/api/apps')])
      const config = await configResponse.json()
      this.webrtcEnabled = config.webrtc_enabled === 'enabled'

      if (appsResponse.ok) {
        const appsBody = await appsResponse.json()
        this.apps = appsBody.apps || []
      }
    } catch (e) {
      console.error('Stream: failed to load configuration', e)
    } finally {
      this.checkedConfig = true
    }
  },
  beforeUnmount() {
    document.removeEventListener('fullscreenchange', this.onFullscreenChange)
    document.removeEventListener('pointerlockchange', this.onPointerLockChange)
    window.removeEventListener('pagehide', this.disconnect)
    window.removeEventListener('keydown', this.onShortcutKeydown)
    this.disconnect()
  },
  methods: {
    async connect() {
      this.error = null
      this.phase = 'connecting'

      const preset = this.resolutionPresets.find((p) => p.key === this.resolutionKey) || this.resolutionPresets[1]
      // Vue wraps everything in data() with a reactive Proxy, which breaks JS private class
      // fields (#foo): a method that does `this.#foo` throws when `this` is the Proxy rather than
      // the real instance. markRaw() keeps these three objects, and only these three, out of that.
      const connection = markRaw(new StreamConnection({ fetch: apiFetch }))
      this.connection = connection
      this.stream = markRaw(new MediaStream())

      connection.onTrack = (track) => {
        this.stream.addTrack(track)
        if (this.$refs.video) {
          this.$refs.video.srcObject = this.stream
        }
      }

      connection.onConnectionStateChange = (state) => {
        if (state === 'connected') {
          this.phase = 'connected'
        } else if (state === 'failed' || state === 'closed') {
          if (this.phase !== 'setup') {
            this.error = this.error || this.$t('stream.connection_lost')
            this.phase = 'failed'
            this.stopControlling()
          }
        }
      }

      connection.onInputChannelOpen = () => {
        // Input capture starts as soon as the channel is ready; whether the browser actually
        // controls the stream (pointer lock etc.) is separate, gated by the "click to control" overlay.
      }

      try {
        await connection.open({
          width: preset.width,
          height: preset.height,
          fps: this.fps,
          bitrate: this.bitrate,
          appid: this.appid,
        })
      } catch (e) {
        this.error = e.message
        this.phase = 'failed'
        return
      }

      this.$nextTick(() => {
        if (this.$refs.video) {
          this.$refs.video.srcObject = this.stream
        }
      })
    },

    startControlling() {
      if (!this.connection || this.controlling) {
        return
      }
      this.controlling = true

      this.input = markRaw(new InputSender({
        send: (text) => this.connection?.sendInput(text),
        sendMotion: (text) => this.connection?.sendMotion(text),
        onAbsoluteMove: (position) => {
          this.localCursor = position
        },
        element: this.$refs.video,
      }))
      this.input.start()
      // Mouse movement is absolute by default, like VNC: the browser's own cursor stays visible and
      // moving it past the edge of the video leaves it free to work with the rest of the page or the
      // local desktop, same as any other embedded picture. Locking the pointer for raw relative motion
      // is a deliberate, separate choice a person makes with the crosshair button, for games that need it.
      // The video's own cursor is hidden (see .stream-hide-cursor) and this local-cursor element is
      // drawn in its place, positioned straight from the browser's own mousemove events rather than
      // waiting on a round trip to the host and back, which is what removes the felt lag on movement.

      this.statsTimer = window.setInterval(async () => {
        if (this.connection) {
          this.stats = await this.connection.stats()
        }
      }, STATS_INTERVAL_MS)
    },

    stopControlling() {
      this.controlling = false
      if (this.input) {
        this.input.stop()
        this.input = null
      }
      if (document.pointerLockElement === this.$refs.video) {
        document.exitPointerLock()
      }
      navigator.keyboard?.unlock?.()
      if (this.statsTimer) {
        window.clearInterval(this.statsTimer)
        this.statsTimer = null
      }
    },

    /**
     * Switch between the default absolute mouse (cursor stays visible and free, like VNC) and a
     * locked, relative one for games that need raw mouse motion rather than a screen position.
     */
    toggleMouseLock() {
      if (!this.input) {
        return
      }
      if (document.pointerLockElement === this.$refs.video) {
        document.exitPointerLock()
      } else {
        this.input.lockPointer()
      }
    },

    onPointerLockChange() {
      this.mouseLocked = document.pointerLockElement === this.$refs.video
    },

    async disconnect() {
      this.stopControlling()
      if (document.fullscreenElement) {
        try {
          await document.exitFullscreen()
        } catch {
          // Nothing more to do if the browser refuses; the page is navigating away or closing anyway.
        }
      }
      if (this.connection) {
        const closing = this.connection
        this.connection = null
        await closing.close()
      }
      this.stream = null
    },

    reset() {
      this.disconnect()
      this.error = null
      this.phase = 'setup'
    },

    async toggleFullscreen() {
      if (document.fullscreenElement) {
        await document.exitFullscreen()
      } else {
        await this.$refs.video?.requestFullscreen?.().catch(() => {})
      }
    },

    toggleStats() {
      this.showStats = !this.showStats
    },

    /**
     * Handle a Moonlight-style Ctrl+Alt+Shift shortcut, if this keystroke is one. Real Ctrl+Alt+Shift
     * combinations are essentially never bound to anything else at the OS or browser level, so this
     * only needs to keep a matched one from also reaching InputSender as a keystroke to forward.
     */
    onShortcutKeydown(event) {
      if (!(event.ctrlKey && event.altKey && event.shiftKey)) {
        return
      }
      const action = SHORTCUT_KEYS[event.code]
      if (!action) {
        return
      }
      event.preventDefault()
      event.stopImmediatePropagation()
      this[action]()
    },

    onFullscreenChange() {
      this.isFullscreen = document.fullscreenElement === this.$refs.video
      if (this.isFullscreen && this.controlling && navigator.keyboard?.lock) {
        navigator.keyboard.lock().catch(() => {
          // Keyboard Lock is an enhancement; streaming still works without it.
        })
      } else if (!this.isFullscreen) {
        navigator.keyboard?.unlock?.()
      }
    },
  },
}
</script>

<style scoped>
/* Once connecting/connected/failed, the page becomes just the stream: no navbar, no page padding,
   filling the whole browser window rather than sitting in a small box under the normal page layout. */
.stream-fullpage {
  position: fixed;
  inset: 0;
  padding: 0;
}

.stream-view {
  position: relative;
  width: 100%;
  height: 100%;
  background: #000;
  overflow: hidden;
}

.stream-video {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
  cursor: pointer;
}

/* While controlling with the mouse unlocked, the local-cursor element below stands in for the video's
   own cursor, which is hidden here so there are never two arrows on screen at once. */
.stream-hide-cursor .stream-video {
  cursor: none;
}

.local-cursor {
  position: fixed;
  z-index: 5;
  pointer-events: none;
  color: #fff;
  filter: drop-shadow(0 0 1px #000) drop-shadow(0 0 2px #000);
  transform: translate(-2px, -2px);
}

.stream-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  color: #fff;
  background: rgba(0, 0, 0, 0.55);
  text-align: center;
  padding: 1rem;
}

.stream-overlay-click {
  cursor: pointer;
}

.stream-toolbar {
  position: absolute;
  top: 0.5rem;
  right: 0.5rem;
  z-index: 10;
  display: flex;
  gap: 0.25rem;
}

.stream-stats {
  position: absolute;
  bottom: 0.5rem;
  left: 0.5rem;
  z-index: 10;
  background: rgba(0, 0, 0, 0.65);
  color: #0f0;
  font-family: monospace;
  font-size: 0.8rem;
  padding: 0.5rem 0.75rem;
  border-radius: 0.25rem;
  line-height: 1.4;
}

.stream-shortcuts-hint {
  margin-top: 1rem;
  font-size: 0.85rem;
  opacity: 0.8;
}

</style>
