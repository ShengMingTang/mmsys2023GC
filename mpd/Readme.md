# Command
## Setup / Clean up
* ```sudo mn -c```
* ```sudo fuser -k 6653/tcp```
* ```python3 GenConfig.py -c 1 -s 1 -p ./testconfig ```
* ```sudo python3 GenTopology.py  -c 1 -s 4 -l 1 -r 4 -p ./testconfig```
## Run
* ```client pathToDownNodeJson [pathToParamJson]```
    * See param.json
```
// param.json
{
    "ccType": 2,
    "multiPathType": 2,
    "minWnd": 1,
    "maxWnd": 64,
    "slowStartThreshold": 32
}
```
## Bin description
* build/bin/ByteDance/MPDtest: initial binary, no modification
* build/bin/MPDtest: latest binary