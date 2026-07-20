<script setup>
import { ref, reactive, onMounted, onBeforeUnmount, computed } from 'vue'
import { RotateCw, Star, Power, RefreshCw, Save, Play } from '@lucide/vue'
import { apiFetch } from '../../../fetch_utils'
import { notifyKey } from '../../../Notification.vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps(['platform', 'config'])
const config = ref(props.config)

const layouts = ref([])
const newLayoutName = ref('')
const supported = ref(false)
const loading = ref(false)
const editableOutputs = ref([])

const CANVAS_MAX_WIDTH = 640
const CANVAS_MAX_HEIGHT = 300
const SNAP_THRESHOLD = 30 // real pixels

const canvasScale = ref(1)
const canvasOffset = reactive({ x: 0, y: 0 })

function effectiveSize(output) {
  const swapped = output.rotation === 90 || output.rotation === 270
  return {
    width: swapped ? output.height : output.width,
    height: swapped ? output.width : output.height,
  }
}

function recomputeCanvasBounds() {
  const enabled = editableOutputs.value.filter((o) => o.enabled)
  if (enabled.length === 0) {
    canvasScale.value = 1
    canvasOffset.x = 0
    canvasOffset.y = 0
    return
  }

  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity
  for (const o of enabled) {
    const { width, height } = effectiveSize(o)
    minX = Math.min(minX, o.x)
    minY = Math.min(minY, o.y)
    maxX = Math.max(maxX, o.x + width)
    maxY = Math.max(maxY, o.y + height)
  }

  const boundWidth = Math.max(1, maxX - minX)
  const boundHeight = Math.max(1, maxY - minY)
  canvasScale.value = Math.min(CANVAS_MAX_WIDTH / boundWidth, CANVAS_MAX_HEIGHT / boundHeight, 0.25)
  canvasOffset.x = minX
  canvasOffset.y = minY
}

const canvasStyle = computed(() => ({
  width: `${CANVAS_MAX_WIDTH}px`,
  height: `${CANVAS_MAX_HEIGHT}px`,
}))

function outputStyle(output) {
  const { width, height } = effectiveSize(output)
  return {
    left: `${(output.x - canvasOffset.x) * canvasScale.value}px`,
    top: `${(output.y - canvasOffset.y) * canvasScale.value}px`,
    width: `${Math.max(width * canvasScale.value, 40)}px`,
    height: `${Math.max(height * canvasScale.value, 40)}px`,
  }
}

let dragState = null

function startDrag(output, event) {
  if (!output.enabled) {
    return
  }
  event.currentTarget.setPointerCapture(event.pointerId)
  dragState = {
    output,
    startClientX: event.clientX,
    startClientY: event.clientY,
    startX: output.x,
    startY: output.y,
  }
  window.addEventListener('pointermove', onDrag)
  window.addEventListener('pointerup', endDrag)
}

function onDrag(event) {
  if (!dragState) {
    return
  }
  const dx = (event.clientX - dragState.startClientX) / canvasScale.value
  const dy = (event.clientY - dragState.startClientY) / canvasScale.value
  dragState.output.x = Math.round(dragState.startX + dx)
  dragState.output.y = Math.round(dragState.startY + dy)
}

function snapOutput(output) {
  const { width, height } = effectiveSize(output)
  const others = editableOutputs.value.filter((o) => o !== output && o.enabled)

  let bestX = null, bestXDist = SNAP_THRESHOLD
  let bestY = null, bestYDist = SNAP_THRESHOLD

  for (const other of others) {
    const otherSize = effectiveSize(other)
    for (const cx of [other.x, other.x - width, other.x + otherSize.width, other.x + otherSize.width - width]) {
      const dist = Math.abs(cx - output.x)
      if (dist < bestXDist) {
        bestXDist = dist
        bestX = cx
      }
    }
    for (const cy of [other.y, other.y - height, other.y + otherSize.height, other.y + otherSize.height - height]) {
      const dist = Math.abs(cy - output.y)
      if (dist < bestYDist) {
        bestYDist = dist
        bestY = cy
      }
    }
  }

  if (bestX !== null) {
    output.x = bestX
  }
  if (bestY !== null) {
    output.y = bestY
  }
}

function endDrag() {
  if (!dragState) {
    return
  }
  snapOutput(dragState.output)
  dragState = null
  recomputeCanvasBounds()
  window.removeEventListener('pointermove', onDrag)
  window.removeEventListener('pointerup', endDrag)
}

onBeforeUnmount(() => {
  window.removeEventListener('pointermove', onDrag)
  window.removeEventListener('pointerup', endDrag)
})

function toggleEnabled(output) {
  output.enabled = !output.enabled
  recomputeCanvasBounds()
}

function rotateOutput(output) {
  output.rotation = (output.rotation + 90) % 360
  recomputeCanvasBounds()
}

function setPrimary(output) {
  for (const o of editableOutputs.value) {
    o.primary = o === output
  }
}

async function loadEditableOutputs() {
  const response = await apiFetch('./api/display/outputs')
  if (response.ok) {
    const data = await response.json()
    editableOutputs.value = (data.outputs || []).map((o) => ({ ...o }))
    recomputeCanvasBounds()
  }
}

async function refreshLayouts() {
  loading.value = true
  try {
    const response = await apiFetch('./api/display/layouts')
    if (response.ok) {
      const data = await response.json()
      layouts.value = data.layouts || []
    }
  } finally {
    loading.value = false
  }
}

async function checkSupported() {
  try {
    const response = await apiFetch('./api/display/outputs')
    if (response.ok) {
      const data = await response.json()
      supported.value = !!data.supported
    }
  } catch (e) {
    console.debug('DisplayLayoutManager: failed to check support', e)
  }
}

async function applyNow() {
  const response = await apiFetch('./api/display/apply', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ outputs: editableOutputs.value }),
  })
  const data = await response.json().catch(() => ({}))
  if (response.ok && data.status) {
    notifyKey.success('config.display_layout_applied')
  } else {
    notifyKey.error('config.display_layout_apply_failed')
  }
}

async function resetCanvas() {
  await loadEditableOutputs()
}

async function saveCurrentLayout() {
  const name = newLayoutName.value.trim()
  if (!name) {
    return
  }

  const response = await apiFetch('./api/display/layouts', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name, outputs: editableOutputs.value }),
  })

  if (response.ok) {
    newLayoutName.value = ''
    notifyKey.success('config.display_layout_saved')
    await refreshLayouts()
  } else {
    notifyKey.error('config.display_layout_save_failed')
  }
}

async function applyLayout(name) {
  const response = await apiFetch('./api/display/layouts/apply', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name }),
  })

  const data = await response.json().catch(() => ({}))
  if (response.ok && data.status) {
    notifyKey.success('config.display_layout_applied')
  } else {
    notifyKey.error('config.display_layout_apply_failed')
  }
}

async function deleteLayout(name) {
  const response = await apiFetch('./api/display/layouts/delete', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name }),
  })

  if (response.ok) {
    await refreshLayouts()
  } else {
    notifyKey.error('config.display_layout_delete_failed')
  }
}

async function toggleRestoreLayout(layout) {
  const name = layout.is_restore ? '' : layout.name
  const response = await apiFetch('./api/display/layouts/set-restore', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name }),
  })

  if (response.ok) {
    await refreshLayouts()
  } else {
    notifyKey.error('config.display_layout_set_restore_failed')
  }
}

onMounted(async () => {
  await checkSupported()
  if (supported.value) {
    await refreshLayouts()
    await loadEditableOutputs()
  }
})
</script>

<template>
  <div v-if="supported" class="mb-3">
    <Checkbox
      id="layout_management_enabled"
      locale-prefix="config"
      v-model="config.layout_management_enabled"
      default="false"
    ></Checkbox>

    <div v-if="config.layout_management_enabled === 'enabled'" class="mt-3">
      <label class="form-label">{{ $t('config.display_layouts') }}</label>
      <div class="form-text">{{ $t('config.display_layouts_desc') }}</div>

      <div class="layout-canvas-container mb-2">
        <div class="layout-canvas" :style="canvasStyle">
          <div v-for="output in editableOutputs" :key="output.id"
               class="layout-canvas-output"
               :class="{ 'is-disabled': !output.enabled, 'is-primary': output.primary }"
               :style="outputStyle(output)"
               @pointerdown="startDrag(output, $event)">
            <div class="output-label">{{ output.friendly_name || output.id }}</div>
            <div class="output-sub">{{ output.width }}x{{ output.height }}<span v-if="output.rotation"> ({{ output.rotation }}°)</span></div>
            <div class="output-controls">
              <button type="button" class="btn btn-sm btn-icon" :title="$t('config.display_layout_toggle_enabled')"
                      @pointerdown.stop @click.stop="toggleEnabled(output)">
                <Power :size="14" />
              </button>
              <button type="button" class="btn btn-sm btn-icon" :title="$t('config.display_layout_rotate')"
                      @pointerdown.stop @click.stop="rotateOutput(output)">
                <RotateCw :size="14" />
              </button>
              <button type="button" class="btn btn-sm btn-icon" :title="$t('config.display_layout_set_primary')"
                      @pointerdown.stop @click.stop="setPrimary(output)">
                <Star :size="14" />
              </button>
            </div>
          </div>
        </div>
      </div>

      <div class="d-flex flex-wrap gap-2 mb-3">
        <button type="button" class="btn btn-primary" @click="applyNow">
          <Play :size="16" /> {{ $t('config.display_layout_apply_now') }}
        </button>
        <button type="button" class="btn btn-outline-secondary" @click="resetCanvas">
          <RefreshCw :size="16" /> {{ $t('config.display_layout_reset') }}
        </button>
      </div>

      <div class="input-group mb-3">
        <input type="text" class="form-control" :placeholder="$t('config.display_layout_name_placeholder')"
               v-model="newLayoutName" @keyup.enter="saveCurrentLayout"/>
        <button type="button" class="btn btn-secondary" @click="saveCurrentLayout">
          <Save :size="16" /> {{ $t('config.display_layout_save_current') }}
        </button>
      </div>

      <ul class="list-group mb-2">
        <li v-for="layout in layouts" :key="layout.name"
            class="list-group-item d-flex justify-content-between align-items-center">
          <span>
            {{ layout.name }}
            <small class="text-body-secondary">
              ({{ layout.outputs.filter(o => o.enabled).length }} {{ $t('config.display_layout_outputs') }})
            </small>
            <span v-if="layout.is_restore" class="badge text-bg-success ms-2">
              {{ $t('config.display_layout_restore_badge') }}
            </span>
          </span>
          <span>
            <button type="button" class="btn btn-sm btn-primary me-2" @click="applyLayout(layout.name)">
              {{ $t('config.display_layout_apply') }}
            </button>
            <button type="button" class="btn btn-sm me-2"
                    :class="layout.is_restore ? 'btn-success' : 'btn-outline-success'"
                    @click="toggleRestoreLayout(layout)">
              {{ layout.is_restore ? $t('config.display_layout_unset_restore') : $t('config.display_layout_set_restore') }}
            </button>
            <button type="button" class="btn btn-sm btn-danger" @click="deleteLayout(layout.name)">
              {{ $t('config.display_layout_delete') }}
            </button>
          </span>
        </li>
        <li v-if="!loading && layouts.length === 0" class="list-group-item text-body-secondary">
          {{ $t('config.display_layouts_empty') }}
        </li>
      </ul>
    </div>
  </div>
</template>

<style scoped>
.layout-canvas-container {
  overflow: auto;
  border: 1px solid var(--bs-border-color);
  border-radius: 0.375rem;
  background: var(--bs-tertiary-bg);
}

.layout-canvas {
  position: relative;
}

.layout-canvas-output {
  position: absolute;
  border: 2px solid var(--bs-primary);
  border-radius: 0.25rem;
  background: var(--bs-body-bg);
  cursor: grab;
  padding: 0.25rem;
  overflow: hidden;
  user-select: none;
  touch-action: none;
  display: flex;
  flex-direction: column;
  justify-content: space-between;
}

.layout-canvas-output:active {
  cursor: grabbing;
}

.layout-canvas-output.is-primary {
  border-color: var(--bs-warning);
  border-width: 3px;
}

.layout-canvas-output.is-disabled {
  opacity: 0.4;
  border-style: dashed;
  cursor: default;
}

.output-label {
  font-size: 0.75rem;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.output-sub {
  font-size: 0.65rem;
  color: var(--bs-secondary-color);
  white-space: nowrap;
}

.output-controls {
  display: flex;
  gap: 0.25rem;
}

.btn-icon {
  padding: 0.1rem 0.3rem;
  line-height: 1;
}
</style>
