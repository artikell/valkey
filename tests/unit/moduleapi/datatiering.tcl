set testmodule [file normalize tests/modules/datatiering.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    
    r set 1 1
    
    r mget 1 2 3
    
    puts [r mget 1 2 3]
}