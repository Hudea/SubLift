import type { BatchTaskStatus } from '../types/batch';

/**
 * 08102 / 08205 / 08308 对齐：批量任务纯状态机与命令可用性守卫
 */
export class BatchTaskStatusGuard {
  /**
   * 纯状态机合法转移判定
   */
  static canTransition(from: BatchTaskStatus, to: BatchTaskStatus): boolean {
    switch (from) {
      case 'waiting':
        return to === 'preparing' || to === 'cancelled' || to === 'skipped' || to === 'interrupted';
      case 'preparing':
        return to === 'extracting' || to === 'failed' || to === 'cancelled' || to === 'interrupted';
      case 'extracting':
        return to === 'exporting' || to === 'failed' || to === 'cancelled' || to === 'interrupted';
      case 'exporting':
        return to === 'completed' || to === 'failed' || to === 'cancelled' || to === 'interrupted';
      case 'completed':
      case 'failed':
      case 'cancelled':
      case 'interrupted':
      case 'skipped':
        return false;
    }
  }

  /**
   * 活动态的主链下一阶段（preparing -> extracting -> exporting；其余 null）
   */
  static nextActiveStage(status: BatchTaskStatus): BatchTaskStatus | null {
    switch (status) {
      case 'preparing':
        return 'extracting';
      case 'extracting':
        return 'exporting';
      default:
        return null;
    }
  }

  /**
   * 是否处于运行活动态
   */
  static isActive(status: BatchTaskStatus): boolean {
    return status === 'preparing' || status === 'extracting' || status === 'exporting';
  }

  /**
   * 是否处于不可变终态
   */
  static isTerminal(status: BatchTaskStatus): boolean {
    return (
      status === 'completed' ||
      status === 'failed' ||
      status === 'cancelled' ||
      status === 'interrupted' ||
      status === 'skipped'
    );
  }

  /**
   * 是否允许单独启动本任务
   */
  static canStartSingle(status: BatchTaskStatus): boolean {
    return status === 'waiting';
  }

  /**
   * 是否允许取消
   */
  static canCancel(status: BatchTaskStatus): boolean {
    return status === 'waiting' || this.isActive(status);
  }

  /**
   * 是否允许重试（仅 failed / cancelled / interrupted 允许重新排队）
   */
  static canRetry(status: BatchTaskStatus): boolean {
    return status === 'failed' || status === 'cancelled' || status === 'interrupted';
  }

  /**
   * 是否允许从队列中移除（非活动态允许）
   */
  static canRemove(status: BatchTaskStatus): boolean {
    return !this.isActive(status);
  }

  /**
   * 是否允许上下移动顺序（仅 waiting 状态允许）
   */
  static canReorder(status: BatchTaskStatus): boolean {
    return status === 'waiting';
  }

  /**
   * 是否允许修改配置（仅 waiting 状态允许）
   */
  static canEditConfig(status: BatchTaskStatus): boolean {
    return status === 'waiting';
  }
}
