# libdash

![cover](https://github.com/WaterDazed/libdash/blob/stable_3_0/doc/cover.png)


主要修改了libdash库的qtsampleplayer 

进行了以下修复：
* 使播放帧率和比例和请求视频相同
* 优化多线程的加锁逻辑，提高稳定性

添加了以下功能：
* 实现倍速播放逻辑，在界面添加切换选框
* 实现快速representaion切换逻辑
* 实现基于吞吐量的带宽估计算法，并基于此实现了自适应算法。隐藏了界面上原有的手动切换representaion的选框

## 在Windows下使用

### 环境

Visual Studio 2010（v100）工具集
Qt 5.4

### 启动

启动libdash.sln