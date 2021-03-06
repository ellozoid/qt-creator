/**************************************************************************
**
** Copyright (C) 2012 - 2014 BlackBerry Limited. All rights reserved.
**
** Contact: BlackBerry (qt@blackberry.com)
** Contact: KDAB (info@kdab.com)
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qnxruncontrolfactory.h"
#include "qnxconstants.h"
#include "qnxrunconfiguration.h"
#include "qnxdebugsupport.h"
#include "qnxanalyzesupport.h"
#include "qnxqtversion.h"
#include "qnxruncontrol.h"
#include "qnxutils.h"
#include "qnxdeviceconfiguration.h"

#include <debugger/debuggerruncontrol.h>
#include <debugger/debuggerrunconfigurationaspect.h>
#include <debugger/debuggerstartparameters.h>
#include <debugger/debuggerkitinformation.h>
#include <analyzerbase/analyzerstartparameters.h>
#include <analyzerbase/analyzermanager.h>
#include <analyzerbase/analyzerruncontrol.h>
#include <analyzerbase/ianalyzertool.h>
#include <projectexplorer/environmentaspect.h>
#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/project.h>
#include <projectexplorer/target.h>
#include <projectexplorer/toolchain.h>
#include <qtsupport/qtkitinformation.h>
#include <utils/portlist.h>

using namespace Analyzer;
using namespace Debugger;
using namespace ProjectExplorer;
using namespace Qnx;
using namespace Qnx::Internal;

static DebuggerStartParameters createDebuggerStartParameters(QnxRunConfiguration *runConfig)
{
    DebuggerStartParameters params;
    Target *target = runConfig->target();
    Kit *k = target->kit();

    const IDevice::ConstPtr device = DeviceKitInformation::device(k);
    if (device.isNull())
        return params;

    params.startMode = AttachToRemoteServer;
    params.debuggerCommand = DebuggerKitInformation::debuggerCommand(k).toString();
    params.sysRoot = SysRootKitInformation::sysRoot(k).toString();
    params.useCtrlCStub = true;
    params.runConfiguration = runConfig;

    if (ToolChain *tc = ToolChainKitInformation::toolChain(k))
        params.toolChainAbi = tc->targetAbi();

    params.executable = runConfig->localExecutableFilePath();
    params.remoteExecutable = runConfig->remoteExecutableFilePath();
    params.remoteChannel = device->sshParameters().host + QLatin1String(":-1");
    params.displayName = runConfig->displayName();
    params.remoteSetupNeeded = true;
    params.closeMode = KillAtClose;
    params.processArgs = runConfig->arguments().join(QLatin1Char(' '));

    DebuggerRunConfigurationAspect *aspect
            = runConfig->extraAspect<DebuggerRunConfigurationAspect>();
    if (aspect->useQmlDebugger()) {
        params.languages |= QmlLanguage;
        params.qmlServerAddress = device->sshParameters().host;
        params.qmlServerPort = 0; // QML port is handed out later
    }

    if (aspect->useCppDebugger())
        params.languages |= CppLanguage;

    if (const Project *project = runConfig->target()->project()) {
        params.projectSourceDirectory = project->projectDirectory().toString();
        if (const BuildConfiguration *buildConfig = runConfig->target()->activeBuildConfiguration())
            params.projectBuildDirectory = buildConfig->buildDirectory().toString();
        params.projectSourceFiles = project->files(Project::ExcludeGeneratedFiles);
    }

    QnxQtVersion *qtVersion =
            dynamic_cast<QnxQtVersion *>(QtSupport::QtKitInformation::qtVersion(k));
    if (qtVersion)
        params.solibSearchPath = QnxUtils::searchPaths(qtVersion);

    return params;
}

static AnalyzerStartParameters createAnalyzerStartParameters(const QnxRunConfiguration *runConfig, RunMode mode)
{
    AnalyzerStartParameters params;
    Target *target = runConfig->target();
    Kit *k = target->kit();

    const IDevice::ConstPtr device = DeviceKitInformation::device(k);
    if (device.isNull())
        return params;

    if (mode == QmlProfilerRunMode)
        params.startMode = StartLocal;
    params.runMode = mode;
    params.debuggee = runConfig->remoteExecutableFilePath();
    params.debuggeeArgs = runConfig->arguments().join(QLatin1Char(' '));
    params.connParams = DeviceKitInformation::device(runConfig->target()->kit())->sshParameters();
    params.displayName = runConfig->displayName();
    params.sysroot = SysRootKitInformation::sysRoot(runConfig->target()->kit()).toString();
    params.analyzerHost = params.connParams.host;
    params.analyzerPort = params.connParams.port;

    if (EnvironmentAspect *environment = runConfig->extraAspect<EnvironmentAspect>())
        params.environment = environment->environment();

    return params;
}

QnxRunControlFactory::QnxRunControlFactory(QObject *parent)
    : IRunControlFactory(parent)
{
}

bool QnxRunControlFactory::canRun(RunConfiguration *runConfiguration, RunMode mode) const
{
    if (mode != NormalRunMode && mode != DebugRunMode && mode != QmlProfilerRunMode)
        return false;

    if (!runConfiguration->isEnabled()
            || !runConfiguration->id().name().startsWith(Constants::QNX_QNX_RUNCONFIGURATION_PREFIX)) {
        return false;
    }

    const QnxRunConfiguration * const rc = qobject_cast<QnxRunConfiguration *>(runConfiguration);
    const QnxDeviceConfiguration::ConstPtr dev = DeviceKitInformation::device(runConfiguration->target()->kit())
            .dynamicCast<const QnxDeviceConfiguration>();
    if (dev.isNull())
        return false;

    if (mode == DebugRunMode || mode == QmlProfilerRunMode)
        return rc->portsUsedByDebuggers() <= dev->freePorts().count();

    return true;
}

RunControl *QnxRunControlFactory::create(RunConfiguration *runConfig, RunMode mode, QString *errorMessage)
{
    Q_ASSERT(canRun(runConfig, mode));

    QnxRunConfiguration *rc = qobject_cast<QnxRunConfiguration *>(runConfig);
    Q_ASSERT(rc);
    switch (mode) {
    case NormalRunMode:
        return new QnxRunControl(rc);
    case DebugRunMode: {
        const DebuggerStartParameters params = createDebuggerStartParameters(rc);
        DebuggerRunControl * const runControl = createDebuggerRunControl(params, errorMessage);
        if (!runControl)
            return 0;

        QnxDebugSupport *debugSupport = new QnxDebugSupport(rc, runControl);
        connect(runControl, SIGNAL(finished()), debugSupport, SLOT(handleDebuggingFinished()));

        return runControl;
    }
    case QmlProfilerRunMode: {
        const AnalyzerStartParameters params = createAnalyzerStartParameters(rc, mode);
        AnalyzerRunControl *runControl = AnalyzerManager::createRunControl(params, runConfig);
        QnxAnalyzeSupport * const analyzeSupport = new QnxAnalyzeSupport(rc, runControl);
        connect(runControl, SIGNAL(finished()), analyzeSupport, SLOT(handleProfilingFinished()));
        return runControl;
    }
    case PerfProfilerRunMode:
    case NoRunMode:
    case CallgrindRunMode:
    case MemcheckRunMode:
    case MemcheckWithGdbRunMode:
    case ClangStaticAnalyzerMode:
    case DebugRunModeWithBreakOnMain:
        QTC_ASSERT(false, return 0);
    }
    return 0;
}
