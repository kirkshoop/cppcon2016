#pragma once


//
// setup up a run_loop scheduler and register a tick 
// function to dispatch from requestAnimationFrame
//

run_loop rl;

auto jsthread = observe_on_run_loop(rl);

void tick(){
    if (!rl.empty() && rl.peek().when < rl.now()) {
        rl.dispatch();
    }
}

composite_subscription lifetime;

void reset() {
    lifetime.unsubscribe();
    lifetime = composite_subscription();
}
