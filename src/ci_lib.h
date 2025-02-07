#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

/* interrupt handler prototype */
typedef void (*ci_handler)(long);

/* prototype of function that can be called before CI is disabled or
 * after CI is enabled in the interrupt handler */
typedef void (*ci_margin_hook)(void);

#ifdef __cplusplus
extern "C" {
#endif

/* for internal use by CI Pass */
extern __thread int LocalLC;
extern __thread int lc_disabled_count;

/******************************************
 *          CI configuration API          *
 * All configurations are thread-specific *
 ******************************************/

/* register interrupt handler */
void register_ci(int, int, ci_handler);

/* de-register interrupt handler */
void deregister_ci(void);

/* register a function to be called just before
 * CI is disabled in the interrupt handler */
void register_ci_disable_hook(ci_margin_hook ci_func);

/* register a function to be called just after
 * CI is enabled in the interrupt handler */
void register_ci_enable_hook(ci_margin_hook ci_func);

/* disable interrupt calls */
void ci_disable(void);

/* enable interrupt calls */
void ci_enable(void);

/* disable probe instrumentation, code should be non-preemptible */
void instr_disable(void);

/* enable probe instrumentation */
void instr_enable(void);

/* for internal use by CI API and CI Pass */

/* CI pass interrupt handler */
extern __thread ci_handler intvActionHook;

/* CI pass IR interrupt interval */
extern __thread uint64_t ci_ir_interval;

/* CI (cycles) pass IR reset interval when the target cycles is not exceeded */
extern __thread uint64_t ci_reset_ir_interval;

/* CI (cycles) pass cycles interrupt interval */
extern __thread uint64_t ci_cycles_interval;

/* CI (cycles) pass cycles interrupt threshold to fire the interrupt or
 * reset the IR counter */
extern __thread uint64_t ci_cycles_threshold;

#ifdef __cplusplus
}
#endif
