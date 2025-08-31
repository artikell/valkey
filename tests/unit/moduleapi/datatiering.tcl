set testmodule [file normalize tests/modules/datatiering.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    r select 0
    
    r set 1 1

    wait_for_log_messages 0 {"*cronLoopCallback evict key*"} 0 10 1000
    
    puts [r mget 1 2 3]
}