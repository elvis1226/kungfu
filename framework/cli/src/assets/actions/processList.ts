import { getAllKfConfigOriginData } from '@kungfu-trader/kungfu-js-api/actions';
import {
  BrokerStateStatusTypes,
  KfCategoryTypes,
} from '@kungfu-trader/kungfu-js-api/typings/enums';
import {
  delayMilliSeconds,
  deleteNNFiles,
  getAvailCliDaemonList,
  getKfExtensionConfig,
  getProcessIdByKfLocation,
  getTaskListFromProcessStatusData,
  kfLogger,
  removeArchiveBeforeToday,
  removeJournal,
} from '@kungfu-trader/kungfu-js-api/utils/busiUtils';
import {
  pm2KillGodDaemon,
  killExtra,
  killKfc,
  pm2Kill,
  Pm2ProcessStatusData,
  Pm2ProcessStatusDetailData,
  sendDataToProcessIdByPm2,
  startArchiveMakeTask,
  killKungfu,
  startMaster,
  startLedger,
  startDzxy,
  startCacheD,
  processStatusDataObservable,
  Pm2Packet,
  Pm2ProcessStatusDetail,
} from '@kungfu-trader/kungfu-js-api/utils/processUtils';
import { combineLatest, filter, map, Observable } from 'rxjs';
import { ProcessListItem } from '../../typings';
import colors from 'colors';
import { Widgets } from 'blessed';
import {
  dealStatus,
  getCategoryName,
  startAllExtDaemons,
} from '../methods/utils';
import { globalState } from '../actions/globalState';
import { dealProcessName } from '../methods/utils';
import {
  ARCHIVE_DIR,
  KF_HOME,
} from '@kungfu-trader/kungfu-js-api/config/pathConfig';

export const mdTdStrategyDaemonObservable = () => {
  return new Observable<
    Record<KfCategoryTypes, KungfuApi.KfConfig[] | KungfuApi.KfDaemonLocation[]>
  >((observer) => {
    Promise.all([getAllKfConfigOriginData(), getAvailCliDaemonList()]).then(
      (
        allConfigs: [
          Record<KfCategoryTypes, KungfuApi.KfConfig[]>,
          KungfuApi.KfDaemonLocation[],
        ],
      ) => {
        observer.next({
          ...allConfigs[0],
          daemon: allConfigs[1].map((item) => ({
            ...item,
            location_uid: 0,
            value: '',
          })),
        });
      },
    );
  });
};

export const appStatesObservable = () => {
  return globalState.DZXY_SUBJECT.pipe(
    filter((packet: Pm2Packet) => {
      if (packet.data.type === 'APP_STATES') {
        return true;
      }
      return false;
    }),
    map((packet: Pm2Packet) => {
      return packet.data.body as Record<string, BrokerStateStatusTypes>;
    }),
  );
};

const getProcessStatus = (
  kfLocation: KungfuApi.KfLocation,
  processStatus: Pm2ProcessStatusData,
  appStates: Record<string, BrokerStateStatusTypes>,
) => {
  const category = kfLocation.category;
  const processId = getProcessIdByKfLocation(kfLocation);
  if (category === 'md' || category === 'td') {
    return dealStatus(
      processStatus[processId]
        ? appStates[processId] || processStatus[processId] || '--'
        : '--',
    );
  } else {
    return dealStatus(processStatus[processId] || '--');
  }
};

export const getExtConfigObservable = () => {
  return new Observable<KungfuApi.KfExtConfigs>((observer) => {
    getKfExtensionConfig().then((kfExtConfigs) => {
      observer.next(kfExtConfigs);
    });
  });
};

export const specificProcessListObserver = (kfLocation: KungfuApi.KfConfig) =>
  combineLatest(
    processStatusDataObservable(),
    appStatesObservable(),
    (
      ps: {
        processStatus: Pm2ProcessStatusData;
        processStatusWithDetail: Pm2ProcessStatusDetailData;
      },
      appStates: Record<string, BrokerStateStatusTypes>,
    ): ProcessListItem[] => {
      const { processStatus, processStatusWithDetail } = ps;
      const processId = getProcessIdByKfLocation(kfLocation);

      return [
        {
          processId: 'master',
          processName: colors.bold('MASTER'),
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'master',
          name: 'master',
          value: {},
          status: processStatus['master'] || '--',
          statusName: dealStatus(processStatus['master'] || '--'),
          monit: processStatusWithDetail['master']?.monit,
        },
        {
          processId: 'cached',
          processName: 'CACHED',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'cached',
          value: {},
          status: processStatus['cached'] || '--',
          statusName: dealStatus(processStatus['cached'] || '--'),
          monit: processStatusWithDetail['cached']?.monit,
        },
        {
          processId: 'ledger',
          processName: 'LEDGER',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'ledger',
          value: {},
          status: processStatus['ledger'] || '--',
          statusName: dealStatus(processStatus['ledger'] || '--'),
          monit: processStatusWithDetail['ledger']?.monit,
        },
        {
          processId: 'dzxy',
          processName: 'DZXY',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'dzxy',
          value: {},
          status: processStatus['dzxy'] || '--',
          statusName: dealStatus(processStatus['dzxy'] || '--'),
          monit: processStatusWithDetail['dzxy']?.monit,
        },
        {
          processId,
          processName: processId,
          typeName: getCategoryName(kfLocation.category),
          category: kfLocation.category,
          group: kfLocation.group,
          name: kfLocation.name,
          value: JSON.parse(kfLocation.value || '{}'),
          status: processStatus[processId] || '--',
          statusName: getProcessStatus(kfLocation, processStatus, appStates),
          monit: processStatusWithDetail[processId]?.monit,
        },
      ];
    },
  );

export const processListObservable = () =>
  combineLatest(
    mdTdStrategyDaemonObservable(),
    processStatusDataObservable(),
    appStatesObservable(),
    getExtConfigObservable(),
    (
      mdTdStrategyDaemon: Record<
        KfCategoryTypes,
        KungfuApi.KfConfig[] | KungfuApi.KfDaemonLocation[]
      >,
      ps: {
        processStatus: Pm2ProcessStatusData;
        processStatusWithDetail: Pm2ProcessStatusDetailData;
      },
      appStates: Record<string, BrokerStateStatusTypes>,
      extConfigs: KungfuApi.KfExtConfigs,
    ): ProcessListItem[] => {
      const { md, td, strategy, daemon } = mdTdStrategyDaemon;
      const { processStatus, processStatusWithDetail } = ps;

      const mdList: ProcessListItem[] = md.map((item) => {
        const processId = getProcessIdByKfLocation(item);
        const prefixProps =
          globalThis.HookKeeper.getHooks().prefix.trigger(item);
        const prefix =
          prefixProps.prefixType === 'text' ? prefixProps.prefix : '';
        return {
          processId,
          processName: prefix + processId,
          typeName: getCategoryName(item.category),
          category: item.category,
          group: item.group,
          name: item.name,
          value: JSON.parse(item.value || '{}'),
          status: processStatus[processId] || '--',
          statusName: dealStatus(
            processStatus[processId]
              ? appStates[processId] || processStatus[processId] || '--'
              : '--',
          ),
          monit: processStatusWithDetail[processId]?.monit,
        };
      });

      const tdList: ProcessListItem[] = td.map((item) => {
        const processId = getProcessIdByKfLocation(item);
        const prefixProps =
          globalThis.HookKeeper.getHooks().prefix.trigger(item);
        const prefix =
          prefixProps.prefixType === 'text' ? prefixProps.prefix : '';
        return {
          processId,
          processName: prefix + processId,
          typeName: getCategoryName(item.category),
          category: item.category,
          group: item.group,
          name: item.name,
          value: JSON.parse(item.value || '{}'),
          status: processStatus[processId] || '--',
          statusName: dealStatus(
            processStatus[processId]
              ? appStates[processId] || processStatus[processId] || '--'
              : '--',
          ),
          monit: processStatusWithDetail[processId]?.monit,
        };
      });

      const strategyList: ProcessListItem[] = strategy.map((item) => {
        const processId = getProcessIdByKfLocation(item);
        const prefixProps =
          globalThis.HookKeeper.getHooks().prefix.trigger(item);
        const prefix =
          prefixProps.prefixType === 'text' ? prefixProps.prefix : '';
        return {
          processId,
          processName: prefix + processId,
          typeName: getCategoryName(item.category),
          category: item.category,
          group: item.group,
          name: item.name,
          value: JSON.parse(item.value || '{}'),
          status: processStatus[processId] || '--',
          statusName: dealStatus(processStatus[processId] || '--'),
          monit: processStatusWithDetail[processId]?.monit,
        };
      });

      const daemonList: ProcessListItem[] = daemon.map((item) => {
        const processId = getProcessIdByKfLocation(item);
        const prefixProps =
          globalThis.HookKeeper.getHooks().prefix.trigger(item);
        const prefix =
          prefixProps.prefixType === 'text' ? prefixProps.prefix : '';
        return {
          processId,
          processName: prefix + (dealProcessName(processId) || processId),
          typeName: getCategoryName(item.category as KfCategoryTypes),
          category: item.category,
          group: item.group,
          name: item.name,
          value: JSON.parse(item.value || '{}'),
          status: processStatus[processId] || '--',
          statusName: dealStatus(processStatus[processId] || '--'),
          monit: processStatusWithDetail[processId]?.monit,
          script: item.script,
          cwd: item.cwd,
        };
      });

      const taskPrefixs: string[] = Object.keys(
        extConfigs['strategy'] || {},
      ).map((key) => `strategy_${key}`);
      const taskList: Pm2ProcessStatusDetail[] =
        getTaskListFromProcessStatusData(
          taskPrefixs,
          ps.processStatusWithDetail,
        );
      const taskListResolved: ProcessListItem[] = taskList.map((item) => {
        return {
          processId: item.name || '',
          processName: `${item.name?.toKfGroup()}_${item.name?.toKfName()}`,
          typeName: colors.magenta('Task'),
          category: 'strategy',
          group: item.name?.toKfGroup() || '',
          name: item.name?.toKfName() || '',
          value: '',
          status: item.status || '--',
          statusName: dealStatus(item.status || '--'),
          monit: item.monit,
          script: item.script,
          cwd: item.cwd,
        };
      });

      const masterPrefixProps = globalThis.HookKeeper.getHooks().prefix.trigger(
        {
          category: 'system',
          group: 'master',
          name: 'master',
          mode: 'live',
        },
      );
      const masterPrefix =
        masterPrefixProps.prefixType === 'text' ? masterPrefixProps.prefix : '';

      return [
        {
          processId: 'archive',
          processName: '_archive_',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: '',
          name: '',
          value: {},
          status: processStatus['archive'] || '--',
          statusName: dealStatus(processStatus['archive'] || '--'),
          monit: processStatusWithDetail['archive']?.monit,
        },
        {
          processId: 'master',
          processName: colors.bold(masterPrefix + 'MASTER'),
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'master',
          name: 'master',
          value: {},
          status: processStatus['master'] || '--',
          statusName: dealStatus(processStatus['master'] || '--'),
          monit: processStatusWithDetail['master']?.monit,
        },
        {
          processId: 'cached',
          processName: 'CACHED',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'cached',
          value: {},
          status: processStatus['cached'] || '--',
          statusName: dealStatus(processStatus['cached'] || '--'),
          monit: processStatusWithDetail['cached']?.monit,
        },
        {
          processId: 'ledger',
          processName: 'LEDGER',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'ledger',
          value: {},
          status: processStatus['ledger'] || '--',
          statusName: dealStatus(processStatus['ledger'] || '--'),
          monit: processStatusWithDetail['ledger']?.monit,
        },
        {
          processId: 'dzxy',
          processName: 'DZXY',
          typeName: colors.bgMagenta('Sys'),
          category: 'system',
          group: 'service',
          name: 'dzxy',
          value: {},
          status: processStatus['dzxy'] || '--',
          statusName: dealStatus(processStatus['dzxy'] || '--'),
          monit: processStatusWithDetail['dzxy']?.monit,
        },
        ...daemonList,
        ...mdList,
        ...tdList,
        ...taskListResolved,
        ...strategyList,
      ];
    },
  );

export const switchProcess = (
  proc: ProcessListItem,
  messageBoard: Widgets.MessageElement,
  loading: Widgets.LoadingElement,
): void => {
  const status = proc.status !== '--';
  const startOrStop = status ? 'Stop' : 'Start';
  const { category, group, name, value, cwd, script } = proc;

  switch (category) {
    case 'system':
      if (proc.processId === 'master') {
        //开启，要归档, cli 需要clearjournal
        preSwitchMain(status, messageBoard, loading)
          .then(() => {
            loading.load(`${startOrStop} Master process`);
            return switchMaster(!status);
          })
          .then(() => {
            loading.stop();
            return messageBoard.log(
              `${startOrStop} Master process success`,
              2,
              (err) => {
                if (err) {
                  console.error(err);
                }
              },
            );
          })
          .catch((err: Error) => kfLogger.error(err));
      } else {
        if (status) {
          messageBoard.log('Stop master first', 2, (err) => {
            if (err) {
              console.error(err);
            }
          });
        } else {
          messageBoard.log(
            'Start master first, If did, Please wait...',
            2,
            (err) => {
              if (err) {
                console.error(err);
              }
            },
          );
        }
      }
      break;
    case 'daemon':
    case 'md':
    case 'td':
    case 'strategy':
      if (!globalState.DZXY_WATCHER_IS_LIVE) {
        messageBoard.log(
          'Start master first, If did, Please wait...',
          2,
          (err) => {
            if (err) {
              console.error(err);
            }
          },
        );
        return;
      }

      sendDataToProcessIdByPm2('SWITCH_KF_LOCATION', globalState.DZXY_PM_ID, {
        category,
        group,
        name,
        value: JSON.stringify(value),
        status,
        cwd,
        script,
      })
        .then(() => {
          messageBoard.log('Please wait...', 2, (err) => {
            if (err) {
              console.error(err);
            }
          });
        })
        .catch((err) => {
          const errMsg = parseSwtchKfLocationErrMessage(err.message);
          messageBoard.log(errMsg, 2, (err) => {
            if (err) {
              console.error(err);
            }
          });
        });
  }
};

function parseSwtchKfLocationErrMessage(errMsg: string): string {
  if (errMsg.includes('offline')) {
    return 'Start master first, If did, Please wait...';
  }

  return errMsg;
}

function preSwitchMain(
  status: boolean,
  message: Widgets.MessageElement,
  loading: Widgets.LoadingElement,
) {
  if (!status) {
    loading.load(`Start Archive, Please wait...`);
    return startArchiveMakeTask().then(() => {
      loading.stop();
      return message.log(`Archive success`, 2, (err) => {
        if (err) {
          console.error(err);
        }
      });
    });
  }

  return Promise.resolve(true);
}

const switchMaster = async (status: boolean): Promise<void> => {
  if (!status) {
    await pm2Kill();
    await pm2KillGodDaemon();
    await killKfc();
    await killExtra();
    if (process.env.NODE_ENV === 'production') {
      await killKungfu();
    }
    await deleteNNFiles();
  } else {
    await removeJournal(KF_HOME);
    await removeArchiveBeforeToday(ARCHIVE_DIR);
    await startMaster(false);
    await delayMilliSeconds(1000);
    await startCacheD(false);
    await delayMilliSeconds(1000);
    await startLedger(false);
    await delayMilliSeconds(1000);
    await startDzxy();
    await delayMilliSeconds(1000);
    await startAllExtDaemons();
  }
};