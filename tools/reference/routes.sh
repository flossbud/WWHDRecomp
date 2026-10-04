# routes.sh: the scripted routes the checks know, by name (sourced by stream_check.sh and timing.sh).
# route_info NAME sets: route (input script in routes/), frames (where the route ends), save (the
# REF_SAVE it starts from, or empty for a fresh boot) and trace (the reference's OS-call trace).
route_info() {
    case "$1" in
        save)  route=continue-100.txt  frames=1800  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/save-det/a.zst ;;
        route) route=title-to-game.txt frames=10800 save=                          trace=/wwhd/data/traces/null-route.zst ;;
        tour)  route=tour-100.txt      frames=2190  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/tour-det/a.zst ;;
        shield) route=shield-100.txt   frames=1320  save=/wwhd/data/saves/wwhd_100 trace= ;;
        ladder) route=ladder-100.txt   frames=1800  save=/wwhd/data/saves/wwhd_100 trace= ;;
        swing) route=swing-100.txt     frames=1260  save=/wwhd/data/saves/wwhd_100 trace= ;;
        land)  route=land-100.txt      frames=2080  save=/wwhd/data/saves/wwhd_100 trace= ;;
        drc)   route=drc-shield.txt    frames=1240  save=/wwhd/data/saves/owner_drc trace= ;;
        bow)   route=bow-100.txt       frames=1040  save=/wwhd/data/saves/wwhd_100 trace= ;;
        sidle) route=drc-sidle.txt     frames=1400  save=/wwhd/data/saves/owner_drc trace= ;;
        door)  route=drc-door.txt      frames=1700  save=/wwhd/data/saves/owner_drc trace= ;;
        plants) route=plants-100.txt   frames=1520  save=/wwhd/data/saves/wwhd_100 trace= ;;
        bk)    route=drc-bk.txt        frames=1300  save=/wwhd/data/saves/owner_drc trace= ;;
        gtower) route=gtower-100.txt   frames=12000  save=/wwhd/data/saves/wwhd_100 trace= ;;
        kazeb) route=kazeb-100.txt     frames=6000  save=/wwhd/data/saves/wwhd_100 trace= ;;
        walk)  route=walk-100.txt      frames=3000  save=/wwhd/data/saves/wwhd_100 trace= ;;
        items) route=items-100.txt     frames=1720  save=/wwhd/data/saves/wwhd_100 trace= ;;
        slash) route=slash-100.txt     frames=1600  save=/wwhd/data/saves/wwhd_100 trace= ;;
        leaf)  route=leaf-100.txt      frames=1200  save=/wwhd/data/saves/wwhd_100 trace= ;;
        carry) route=carry-100.txt     frames=1120  save=/wwhd/data/saves/wwhd_100 trace= ;;
        crawl) route=crawl-100.txt     frames=1200  save=/wwhd/data/saves/wwhd_100 trace= ;;
        pot)   route=drc-pot.txt       frames=1440  save=/wwhd/data/saves/owner_drc trace= ;;
        back)  route=back-100.txt      frames=1400  save=/wwhd/data/saves/wwhd_100 trace= ;;
        door2) route=door2-100.txt     frames=1400  save=/wwhd/data/saves/wwhd_100 trace= ;;
        hook)  route=hook-100.txt      frames=1200  save=/wwhd/data/saves/wwhd_100 trace= ;;
        sail)  route=sail-100.txt      frames=2940  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/sail-det/a.zst ;;
        menus) route=menus-100.txt     frames=1920  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/menus-det/a.zst ;;
        warp)  route=warp-100.txt      frames=3780  save=/wwhd/data/saves/wwhd_100 trace=/wwhd/data/traces/warp-det/a.zst ;;
        *) return 1 ;;
    esac
}
route_names="save route tour sail menus warp shield ladder swing land drc pot bow sidle door bk plants gtower kazeb walk items slash leaf crawl carry back door2 hook"
