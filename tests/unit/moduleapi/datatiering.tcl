set testmodule [file normalize tests/modules/datatiering.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    
    r mget 1 2 3
    
    wait_for_log_messages 0 {"*DataTieringFilter callback*"} 0 10 1000
}