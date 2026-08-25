<template>
  <!-- 1. 服务初始化连接中 -->
  <div v-if="!systemStore.isReady && systemStore.isLoading" class="sl-app-loading" role="status" aria-live="polite">
    <div class="sl-loading-spinner" aria-hidden="true"></div>
    <span class="sl-loading-text">正在连接 SubLift 核心服务…</span>
  </div>

  <!-- 2. 引导配置屏（未配置工作区目录时） -->
  <WorkspaceSetupView v-else-if="!systemStore.isWorkspaceConfigured" />

  <!-- 3. 主应用外壳（已配置工作区目录） -->
  <div v-else class="sl-app-shell">
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
import WorkspaceSetupView from './views/WorkspaceSetupView.vue';
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

.sl-app-loading {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 100vw;
  height: 100vh;
  background-color: var(--sl-surface-ground);
  gap: 16px;
}

.sl-loading-spinner {
  width: 32px;
  height: 32px;
  border: 3px solid rgba(255, 255, 255, 0.1);
  border-top-color: var(--sl-color-accent);
  border-radius: 50%;
  animation: sl-spin 0.8s linear infinite;
}

@keyframes sl-spin {
  to {
    transform: rotate(360deg);
  }
}

.sl-loading-text {
  font-size: var(--sl-font-size-base);
  color: var(--sl-text-secondary);
}

.sl-app-main {
  flex: 1;
  min-height: 0;
  position: relative;
  overflow: hidden;
}
</style>
