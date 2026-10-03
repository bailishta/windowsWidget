#pragma once

namespace companion {
// Preview is a temporary entry point, not a fallback after following Shell.
// Layout preferences never change this display session.
struct FollowSession {
    bool preview=false;
    bool adopt_panel() {
        bool consumed=preview;
        preview=false;
        return consumed;
    }
    bool standalone(bool editing) const {return preview || editing;}
};
}
