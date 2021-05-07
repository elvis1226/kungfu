
import moment from 'moment';

import { debounce } from '__gUtils/busiUtils';
import { KF_DATASET_QUOTE_DIR } from '__gConfig/pathConfig';
import { buildInstrumentsPipeByDeamon } from '@/ipcMsg/deamon';

import Workers from '@/workers/index';

export default {

    mounted () {

        this.bindInstrumentsDataListener();
        this.bindInstrumentWorkerListener();
        this.bindAvgMarketVolumeListener();
    },

    destroyed () {
        this.instrumentsDataPipe && this.instrumentsDataPipe.unsubscribe()
    },

    methods: {
        bindInstrumentsDataListener () {
            this.instrumentsDataPipe = buildInstrumentsPipeByDeamon().subscribe(data => {
                if (this.getIfSaveInstruments()) {
                    const instruments = data['instruments'] || [];
                    if (!instruments.length) return;
                    localStorage.setItem('instrumentsSavedDate', moment().format('YYYY-MM-DD-HH-mm'))
                    this.saveInstrumentsIntoLocalstorage(instruments)
                }
            })
        },

        bindInstrumentWorkerListener () {
            const self = this;
            Workers.insturmnetWorker.onmessage = debounce(function (event) {
                const instrumentsResolved = event.data || [];

                if (!instrumentsResolved.length) {
                    localStorage.setItem('instrumentsSavedDate', '')
                    return;
                };

                localStorage.setItem('instruments', JSON.stringify(instrumentsResolved))
                self.$bus.$emit('update:instruments')
            }, 1000);
        },

        bindAvgMarketVolumeListener () {
            const self = this;
            Workers.calcMarketDataAvgVolumeWorker.postMessage({
                days: 7,
                dataPath: KF_DATASET_QUOTE_DIR
            })

            Workers.calcMarketDataAvgVolumeWorker.onmessage = debounce(function (event) {{
                self.$store.dispatch('setMarketAvgVolume', event.data)
            }})
        },

        getIfSaveInstruments () {
            const instrumentsSavedDate = localStorage.getItem('instrumentsSavedDate')
            if (!instrumentsSavedDate) {
                return true
            } else if (instrumentsSavedDate !== moment().format('YYYY-MM-DD-HH-mm')) {
                return true 
            } else {
                return false
            }
        },

        saveInstrumentsIntoLocalstorage (instruments) {
            if (!instruments.length) return;

            // 0 Unknown,     //未知
            // 1 Stock,       //普通股票
            // 2 Future,      //期货
            // 3 Bond,        //债券
            // 4 tockOption, //股票期权
            // 5 Fund,        //基金
            // 6 TechStock,   //科创板股票
            // 7 Index,       //指数
            // 8 Repo         //回购

            const instrumentsResolved = instruments
                .filter(item => {
                    //普通股票 期货 股票期权 基金 科创板股票 指数
                    if (item.instrument_type === 1) return true;
                    if (item.instrument_type === 2) return true;
                    if (item.instrument_type === 4) return true;
                    if (item.instrument_type === 5) return true;
                    if (item.instrument_type === 6) return true;
                    if (item.instrument_type === 7) return true;

                    return false
                })

            Workers.insturmnetWorker.postMessage(instrumentsResolved);
        }
    }
}