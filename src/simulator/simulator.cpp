/***************************************************************************
 *   Copyright (C) 2012 by santiago González                               *
 *   santigoro@gmail.com                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.  *
 *                                                                         *
 ***************************************************************************/
 
#include <iostream>

#include "simulator.h"
#include "matrixsolver.h"
#include "e-element.h"
#include "oscopewidget.h"
#include "plotterwidget.h"
#include "outpaneltext.h"
#include "mcucomponent.h"
#include "mainwindow.h"


Simulator* Simulator::m_pSelf = 0l;

Simulator::Simulator( QObject* parent ) : QObject(parent)
{
    m_pSelf = this;

    m_isrunning = false;
    m_paused    = false;
    //m_changed   = false;

    m_step       = 0;
    m_numEnodes  = 0;
    m_timerId    = 0;
    m_lastStep   = 0;
    m_lastRefTime = 0;
    m_stepsPrea  = 50;
    m_stepsNolin = 10;
    m_mcuStepsPT = 16;
    m_simuRate   = 1000000;
    
    //QThread      matrixThread;
    //m_matrix.moveToThread( &m_mcuThread );
    
    //m_avr.moveToThread( &m_mcuThread );
    //m_avrCpu = 0l;
    
#ifndef NO_PIC
    //m_pic.moveToThread( &m_mcuThread );
    //m_picCpu = 0l;
#endif
    //m_mcuThread.start();
    //matrixThread.start();
    m_RefTimer.start();
}
Simulator::~Simulator() 
{ 
    m_CircuitFuture.waitForFinished();
    //m_mcuThread.quit();
    //Sm_mcuThread.wait();
    //m_mcuThread.deletelater();
    //m_RefTimer.invalidate();
}

inline void Simulator::solveMatrix()
{
    foreach( eNode* node,  m_eChangedNodeList ) node->stampMatrix();
    m_eChangedNodeList.clear();
    
    if( !m_matrix.solveMatrix() )            // Try to solve matrix,
    {                                     // if fail stop simulation
        std::cout << "Simulator::runStep, Failed to solve Matrix" << std::endl;
        MainWindow::self()->powerCircOff();//stopSim();
        MainWindow::self()->setRate( -1 );
    }                                // m_matrix sets the eNode voltages
}

void Simulator::timerEvent( QTimerEvent* e )  //update at m_timerTick rate (50 ms, 20 Hz max)
{
    e->accept();
    if( !m_isrunning ) return;
    
    // Run Circuit in a parallel thread
    m_CircuitFuture.waitForFinished();
    foreach( eElement* el, m_updateList ) el->updateStep();
    m_CircuitFuture = QtConcurrent::run( this, &Simulator::runCircuit );
    //runCircuit();

    // Get Real Simulation Speed
    qint64 refTime      = m_RefTimer.nsecsElapsed();
    qint64 deltaRefTime = refTime-m_lastRefTime;  

    if( deltaRefTime >= 1e9 )          // We want steps per Sec = 1e9 ns
    {
        qint64 stepsPerSec = (m_step-m_lastStep)*1e9/deltaRefTime;
        MainWindow::self()->setRate( (stepsPerSec*100)/m_simuRate );
        m_lastStep    = m_step;
        m_lastRefTime = refTime;
    }
    // Run Graphic Elements
    PlotterWidget::self()->step();
    OscopeWidget::self()->step();
    TerminalWidget::self()->step();
}


void Simulator::runCircuit()
{
    for( int i=0; i<m_circuitRate; i++ )
    {
        if( !m_isrunning ) return;
        m_step ++;
        
        // Run Reactive Elements
        m_stage = RunStage_reactive;
        if( ++m_reacCounter == m_stepsPrea )
        {
            m_reacCounter = 0;
            
            foreach( eElement* el, m_reactiveList ) el->setVChanged();
            m_reactiveList.clear();
        }
        // Run Sinchronized to Simulation Clock elements
        m_stage = RunStage_clock;
        foreach( eElement* el, m_simuClock ) el->setVChanged();
        OscopeWidget::self()->setData();
        
        // Run Fast elements
        m_stage = RunStage_fast;
        foreach( eElement* el, m_changedFast ) el->setVChanged();
        m_changedFast.clear();
        
        if( !m_eChangedNodeList.isEmpty() ) { solveMatrix(); }
        if( !m_isrunning ) return;
        // Run Mcus
        //foreach( BaseProcessor* proc, m_mcuList ) proc->step();
        if( BaseProcessor::self() ) BaseProcessor::self()->step();
        //if( m_avrCpu ) m_avr.step(); //m_avrCpu->run(m_avrCpu);//avr_run( m_avrCpu ); //avr.step();
    #ifndef NO_PIC
        //if( m_picCpu ) m_pic.step();
    #endif

        m_stage = RunStage_nonLinear;
        // Run Non-Linear elements
        if( ++m_noLinCounter == m_stepsNolin )
        {
            m_noLinCounter = 0;
            int counter = 0;
            while( !m_nonLinear.isEmpty() ) // Run untill all converged
            {
                foreach( eElement* el, m_nonLinear ) el->setVChanged();
                m_nonLinear.clear();

                if( !m_eChangedNodeList.isEmpty() ) { solveMatrix(); }
                if( !m_isrunning ) return;

                if( ++counter > 20 ) break; // Limit the number of loops
            }
            //if( counter > 0 ) qDebug() << "\nSimulator::runCircuit  Non-Linear Solved in steps:"<<counter;
        }
    }
 }
 
void Simulator::runExtraStep()
{
    if( !m_eChangedNodeList.isEmpty() ) { solveMatrix(); }
    
    // Run Fast elements
    foreach( eElement* el, m_changedFast ) el->setVChanged();
    m_changedFast.clear();
}

void Simulator::runContinuous()
{
    m_nonLinear.clear();
    m_changedFast.clear();
    m_reactiveList.clear();
    m_eChangedNodeList.clear();

    foreach( eElement* el, m_elementList )    // Initialize all Elements
    {
        
        if( !m_paused ) 
        {
            //std::cout << "el->resetState()"
            //      <<  std::endl;
            el->resetState();
            m_paused = false;
        }
        el->initialize();
    }
    // Initialize Matrix
    m_matrix.createMatrix( m_eNodeList, m_elementList );
    m_matrix.simplify();

    // Try to solve matrix, if fail stop simulation
    if( !m_matrix.solveMatrix() )                
    {
        std::cout << "Simulator::runContinuous, Failed to solve Matrix"
                  <<  std::endl;
        MainWindow::self()->powerCircOff();
        MainWindow::self()->setRate( -1 );
        //pauseSim();
        return;
    }
    //m_matrix.printMatrix();
    
    m_reacCounter  = 0;
    m_noLinCounter = 0;
    
    simuRateChanged( m_simuRate );
    
    m_isrunning = true;
    std::cout << "\n    Running \n"<<std::endl;
    m_timerId = this->startTimer( m_timerTick );
}

void Simulator::stopSim()
{
    OscopeWidget::self()->clear();

    if( !m_isrunning ) return;
    
    stopTimer();
    
    foreach( eNode* node,  m_eNodeList  )  node->setVolt( 0 ); 
    foreach( eElement* el, m_elementList )
    {
        el->initialize();
        el->resetState();
    }
    foreach( eElement* el, m_updateList )  el->updateStep();

    if( McuComponent::self() ) McuComponent::self()->reset();

    MainWindow::self()->setRate( 0 );
}

void Simulator::pauseSim()
{
    stopTimer();
    m_paused = true;
}

void Simulator::stopTimer()
{
    m_CircuitFuture.waitForFinished();
    
    if( m_timerId != 0 )
    {
        this->killTimer( m_timerId );
        m_timerId = 0;
    }
    m_isrunning = false;
    std::cout << "\n    Simulation Stopped \n" << std::endl;
}

void Simulator::resumeSim()
{
    std::cout << "\n    Resuming Simulation\n" << std::endl;
    m_timerId = this->startTimer( m_timerTick );
    m_isrunning = true;
}

int Simulator::simuRateChanged( int rate )
{
    if( rate > 1e6 ) rate = 1e6;
    if( rate < 1 )   rate = 1;
    
    m_timerTick  = 50;
    int fps = 1000/m_timerTick;
    
    m_circuitRate = rate/fps;
    
    if( rate < fps )
    {
        fps = rate;
        m_circuitRate = 1;
        m_timerTick = 1000/rate;
    }

    if( m_isrunning ) 
    {
        pauseSim();
        resumeSim();
    }
    PlotterWidget::self()->setPlotterTick( m_circuitRate*20 );
    
    m_simuRate = m_circuitRate*fps;
    
    std::cout << "\nFPS:              " << fps
              << "\nCircuit Rate:     " << m_circuitRate
              << "\nMcu     Rate:     " << m_mcuStepsPT
              << std::endl
              << "\nSimulation Speed: " << m_simuRate
              /*<< "\nReactive   Speed: " << m_simuRate/m_stepsPrea*/
              << "\nReactive SubRate: " << m_stepsPrea
              << "\nNoLinear Subrate: " << m_stepsNolin
              << std::endl;

    
    return m_simuRate;
}

/*int  Simulator::simuRate()   
{ 
    return m_simuRate; 
}*/

/*void Simulator::setChanged( bool changed ) 
{ 
    m_changed = changed; 
}*/

/*void Simulator::setNodeVolt( int enode, double v ) 
{ 
    if( isnan(v) )
    {
        //qDebug() << "Simulator::setNodeVolt NaN";
        MainWindow::self()->setRate( -1 );
        return;
    }
    m_eNodeList.at(enode)->setVolt( v ); 
}*/

bool Simulator::isRunning()  
{ 
    return m_isrunning; 
}

int Simulator::reaClock()  
{ 
    return m_stepsPrea;
}

void Simulator::setReaClock( int value )
{
    bool running = m_isrunning;
    if( running ) stopSim();
    
    if     ( value < 3  ) value = 3;
    else if( value > 50 ) value = 50;
    
    m_stepsPrea = value;
    
    if( running ) runContinuous();
}

int  Simulator::noLinClock()
{
    return m_stepsNolin;
}

void Simulator::setNoLinClock( int value )
{
    bool running = m_isrunning;
    if( running ) stopSim();

    if     ( value < 1  ) value = 1;
    else if( value > 50 ) value = 50;

    m_stepsNolin = value;

    if( running ) runContinuous();
}
void Simulator::setMcuClock( int value )
{
    m_mcuStepsPT = value;
    BaseProcessor::self()->setSteps( value );
    //m_pic.setSteps( value );
}

//int  Simulator::stepsPT()    { return m_reaStepsPT; }

unsigned long long Simulator::step() 
{ 
    return m_step; 
}

void Simulator::addToEnodeList( eNode* nod )
{
    if( !m_eNodeList.contains(nod) ) m_eNodeList.append( nod );
}

void Simulator::remFromEnodeList( eNode* nod, bool del )
{
    if( m_eNodeList.contains(nod) )m_eNodeList.removeOne( nod );

    if( del ){ delete nod; }
}

void Simulator::addToChangedNodeList( eNode* nod )
{
    if( !m_eChangedNodeList.contains(nod) ) m_eChangedNodeList.append( nod );
}
void Simulator::remFromChangedNodeList( eNode* nod ) 
{ 
    m_eChangedNodeList.removeOne( nod ); 
}

void Simulator::addToElementList( eElement* el )
{
    if( !m_elementList.contains(el) ) m_elementList.append(el);
}

void Simulator::remFromElementList( eElement* el )
{
    if( m_elementList.contains(el) )m_elementList.removeOne(el);
}

void Simulator::addToUpdateList( eElement* el )
{
    if( !m_updateList.contains(el) ) m_updateList.append(el);
}

void Simulator::remFromUpdateList( eElement* el )
{
    m_updateList.removeOne(el); 
}

void Simulator::addToSimuClockList( eElement* el )   
{ 
    if( !m_simuClock.contains(el) ) m_simuClock.append(el); 
}

void Simulator::remFromSimuClockList( eElement* el ) 
{ 
    m_simuClock.removeOne(el); 
}

void Simulator::addToChangedFast( eElement* el )   
{ 
    if( !m_changedFast.contains(el) ) m_changedFast.append(el); 
}

void Simulator::remFromChangedFast( eElement* el ) 
{ 
    m_changedFast.removeOne(el); 
}

void Simulator::addToReactiveList( eElement* el )
{ 
    if( !m_reactiveList.contains(el) ) m_reactiveList.append(el);
}

void Simulator::remFromReactiveList( eElement* el )
{ 
    m_reactiveList.removeOne(el);
}

void Simulator::addToNoLinList( eElement* el )
{
    if( !m_nonLinear.contains(el) ) m_nonLinear.append(el);
}

void Simulator::remFromNoLinList( eElement* el )
{
    m_nonLinear.removeOne(el);
}

void Simulator::addToMcuList( BaseProcessor* proc )
{
    if( !m_mcuList.contains(proc) ) m_mcuList.append( proc );
}
void Simulator::remFromMcuList( BaseProcessor* proc ) { m_mcuList.removeOne( proc ); }

#include "moc_simulator.cpp"
