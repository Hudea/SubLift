<template>
  <div class="sl-app-shell">
    <Navbar />
    <WorkbenchView />
  </div>
</template>

<script setup lang="ts">
import { onMounted } from 'vue';
import Navbar from './components/Navbar.vue';
import WorkbenchView from './views/WorkbenchView.vue';
import { useSystemStore } from './stores/system';

const systemStore = useSystemStore();

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
</style>
