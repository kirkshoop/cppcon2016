#pragma once

extern"C" {
    void rxmousedrags();
}

void rxmousedrags()
{
    auto down$ = mousedown$("#window").publish().connect_forever();
    auto up$ = mouseup$("#window").publish().connect_forever();
    auto move$ = mousemove$("#window").publish().connect_forever();

    lifetime.add(
        down$.
            map([=](MouseEvent){
                return move$.
                    take_until(up$).
                    map([](MouseEvent){return 1;}).
                    start_with(0).
                    sum();
            }).
            merge().
            subscribe( 
                [](int count){cout << count << " moves while mouse down" << endl;},
                [](exception_ptr ep){cout << what(ep) << endl;}));
}
