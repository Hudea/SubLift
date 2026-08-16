<template>
  <div class="sl-app-shell">
    <Navbar :current-view="currentView" @update:view="currentView = $event" />
    <main class="sl-app-main">
      <WorkbenchView v-show="currentView === 'workbench'" />
      <TaskCenterView v-show="currentView === 'batch'" />
    </main>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue';
import Navbar from './components/Navbar.vue';
import WorkbenchView from './views/WorkbenchView.vue';
import TaskCenterView from './views/TaskCenterView.vue';
import { useSystemStore } from './stores/system';

const systemStore = useSystemStore();
const currentView = ref<'workbench' | 'batch'>('workbench');

onMounted(() => {
  systemStore.fetchSystemInfo();
});
</script>

<style scoped>
.sl-app-shell {
  display: flex;
  flex-direction: column;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  background-color: var(--sl-surface-ground);
}

.sl-app-main {
  flex: 1;
  min-height: 0;
  position: relative;
  overflow: hidden;
}
</style>
