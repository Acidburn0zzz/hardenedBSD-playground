
#include <linux/ww_mutex.h>
#include <linux/wait.h>
#include <linux/spinlock.h>

static inline int
lock_check_stamp(struct mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
	struct ww_acquire_ctx *hold_ctx = READ_ONCE(ww->ctx);

	if (hold_ctx == NULL)
		return (0);

	if (__predict_false(ctx == hold_ctx))
		return (-EALREADY);

	if (ctx->stamp - hold_ctx->stamp <= LONG_MAX &&
	    (ctx->stamp != hold_ctx->stamp || ctx > hold_ctx)) {
		return (-EDEADLK);
	}

	return (0);
}

int
_linux_mutex_lock_common(struct mutex *lock, int state, struct ww_acquire_ctx *ctx, char *file, int line)
{
	struct sx *sx;
	struct ww_mutex *ww;
	int rc;
	sx = &lock->sx;
	if (SKIP_SLEEP())
		return (0);

	if (ctx && ctx->acquired > 0) {
		if ((rc = lock_check_stamp(lock, ctx)))
			return (rc);
	}
	if (state == TASK_UNINTERRUPTIBLE)
		_sx_xlock(&lock->sx, 0, file, line);
	else if (state == TASK_INTERRUPTIBLE)
		_sx_xlock(&lock->sx, SX_INTERRUPTIBLE, file, line);
	else
		panic("unknown state %d", state);

	if (ctx) {
		ww = container_of(lock, struct ww_mutex, base);
		ctx->acquired++;
		ww->ctx = ctx;
	}

	return (0);
}
