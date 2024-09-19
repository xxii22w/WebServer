# WebServer

## 运行
` make `
` ./bin/server `

网页访问 127.0.0.1:端口号

目前添加了内存池和LRU，但是目前没有加进代码，后续可能一些组件会放入pool，makefile是将文件的所有加入编译的，因为这些组件没有添加进去，所以make不出来，可以自己修改makefile

学习自这个项目 : https://github.com/markparticle/WebServer