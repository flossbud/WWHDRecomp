# routes.sh: the scripted routes the checks know, by name (sourced by stream_check.sh and timing.sh).
# route_info NAME sets: route (input script in routes/), frames (where the route ends), save (the
# REF_SAVE it starts from, or empty for a fresh boot) and trace (the reference's OS-call trace).
route_info() {
    case "$1" in
        save)  route=continue-100.txt  frames=1800  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/save-det/a.zst ;;
        route) route=title-to-game.txt frames=10800 save=                          trace=/wwhd/data/traces/null-route.zst ;;
        tour)  route=tour-100.txt      frames=2190  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/tour-det/a.zst ;;
        sail)  route=sail-100.txt      frames=2940  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/sail-det/a.zst ;;
        menus) route=menus-100.txt     frames=1920  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/menus-det/a.zst ;;
        warp)  route=warp-100.txt      frames=3780  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/warp-det/a.zst ;;
        *) return 1 ;;
    esac
}
route_names="save route tour sail menus warp"
