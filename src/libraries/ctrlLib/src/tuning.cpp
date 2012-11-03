/* 
 * Copyright (C) 2012 Department of Robotics Brain and Cognitive Sciences - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <algorithm>
#include <string>
#include <sstream>
#include <stdio.h>

#include <gsl/gsl_math.h>

#include <yarp/math/Math.h>
#include <yarp/math/SVD.h>
#include <iCub/ctrl/tuning.h>

using namespace std;
using namespace yarp::os;
using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::math;
using namespace iCub::ctrl;


/**********************************************************************/
OnlineDCMotorEstimator::OnlineDCMotorEstimator()
{
    Vector x0(4,0.0);
    x0[2]=x0[3]=1.0;
    init(0.01,1.0,1.0,1e5,x0);
}


/**********************************************************************/
bool OnlineDCMotorEstimator::init(const double Ts, const double Q,
                                  const double R, const double P0,
                                  const Vector &x0)
{
    if (x0.length()<4)
        return false;

    A=F=eye(4,4);
    B.resize(4,0.0);
    C.resize(1,4); C(0,0)=1.0;
    Ct=C.transposed();

    P=P0*eye(4,4);
    this->Q=Q*eye(4,4);
    this->R=R;

    this->Ts=Ts;
    x=_x=x0.subVector(0,3);
    x[2]=1.0/_x[2];
    x[3]=_x[3]/_x[2];

    return true;
}


/**********************************************************************/
bool OnlineDCMotorEstimator::init(const double P0, const Vector &x0)
{
    if (x0.length()<4)
        return false;

    P=P0*eye(4,4);
    x=_x=x0.subVector(0,3);
    x[2]=1.0/_x[2];
    x[3]=_x[3]/_x[2];

    return true;
}


/**********************************************************************/
Vector OnlineDCMotorEstimator::estimate(const double u, const double y)
{
    double &x1=x[0];
    double &x2=x[1];
    double &x3=x[2];
    double &x4=x[3];

    double _exp=exp(-Ts*x3);
    double _exp_1=1.0-_exp;
    double _x3_2=x3*x3;
    double _tmp_1=(Ts*x3-_exp_1)/_x3_2;

    A(0,1)=_exp_1/x3;
    A(1,1)=_exp;

    B[0]=x4*_tmp_1;
    B[1]=x4*A(0,1);

    F(0,1)=A(0,1);
    F(1,1)=A(1,1);

    F(0,2)=-(x2*_exp_1)/_x3_2   + (u*x4*Ts*_exp_1)/_x3_2 - (2.0*u*B[0])/x3 + (Ts*x2*_exp)/x3;
    F(1,2)=-(u*x4*_exp_1)/_x3_2 - Ts*x2*_exp             + (u*x4*Ts*_exp)/x3;

    F(0,3)=u*_tmp_1;
    F(1,3)=u*A(0,1);

    // prediction
    x=A*x+B*u;
    P=F*P*F.transposed()+Q;

    // Kalman gain
    Matrix tmpMat=C*P*Ct;
    Matrix K=P*Ct/(tmpMat(0,0)+R);

    // correction
    x+=K*(y-C*x);
    P=(eye(4,4)-K*C)*P;

    _x[0]=x[0];
    _x[1]=x[1];
    _x[2]=1.0/x[2];
    _x[3]=x[3]/x[2];

    return _x;
}


/**********************************************************************/
OnlineStictionEstimator::OnlineStictionEstimator() :
                         RateThread(1000),   velEst(32,4.0), accEst(32,4.0),
                         trajGen(1,1.0,1.0), intErr(1.0,Vector(2,0.0))
{
    imod=NULL;
    ilim=NULL;
    ienc=NULL;
    ipid=NULL;
    configured=false;
}


/**********************************************************************/
bool OnlineStictionEstimator::configure(PolyDriver &driver, const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (driver.isValid() && opt.check("joint"))
    {
        bool ok=true;
        ok&=driver.view(imod);
        ok&=driver.view(ilim);
        ok&=driver.view(ienc);
        ok&=driver.view(ipid);

        if (!ok)
            return false;

        joint=opt.find("joint").asInt();
        setRate((int)(1000.0*opt.check("Ts",Value(0.01)).asDouble()));

        T=opt.check("T",Value(2.0)).asDouble();
        Kp=opt.check("Kp",Value(10.0)).asDouble();
        Ki=opt.check("Ki",Value(250.0)).asDouble();
        Kd=opt.check("Kd",Value(15.0)).asDouble();
        vel_thres=fabs(opt.check("vel_thres",Value(5.0)).asDouble());
        e_thres=fabs(opt.check("e_thres",Value(1.0)).asDouble());

        gamma.resize(2,0.001);
        if (Bottle *pB=opt.find("gamma").asList()) 
        {
            size_t len=std::min(gamma.length(),(size_t)pB->size());
            for (size_t i=0; i<len; i++)
                gamma[i]=pB->get(i).asDouble();
        }

        stiction.resize(2,0.0);
        if (Bottle *pB=opt.find("stiction").asList()) 
        {
            size_t len=std::min(stiction.length(),(size_t)pB->size());
            for (size_t i=0; i<len; i++)
                stiction[i]=pB->get(i).asDouble();
        }

        return configured=true;
    }
    else
        return false;
}


/**********************************************************************/
bool OnlineStictionEstimator::threadInit()
{
    if (!configured)
        return false;

    ilim->getLimits(joint,&x_min,&x_max);
    double x_range=x_max-x_min;
    x_min+=0.1*x_range;
    x_max-=0.1*x_range;
    imod->setOpenLoopMode(joint);

    ienc->getEncoder(joint,&x_pos);
    x_vel=0.0;
    x_acc=0.0;

    tg=x_min;
    xd_pos=x_pos;
    state=(tg-x_pos>0.0)?rising:falling;
    adapt=adaptOld=false;

    trajGen.setTs(0.001*getRate());
    trajGen.setT(T);
    trajGen.init(Vector(1,x_pos));

    Vector _Kp(1,Kp),  _Ki(1,Ki),  _Kd(1,Kd);
    Vector _Wp(1,1.0), _Wi(1,1.0), _Wd(1,1.0);
    Vector _N(1,10.0), _Tt(1,1.0);

    Pid pidInfo;
    ipid->getPid(joint,&pidInfo);
    dpos_dV=(pidInfo.kp>=0.0?-1.0:1.0);
    Matrix _satLim(1,2);
    _satLim(0,0)=-pidInfo.max_int;
    _satLim(0,1)=pidInfo.max_int;

    pid=new parallelPID(0.001*getRate(),_Kp,_Ki,_Kd,_Wp,_Wi,_Wd,_N,_Tt,_satLim);
    pid->reset(Vector(1,0.0));

    intErr.setTs(0.001*getRate());
    intErr.reset(stiction);

    done=0.0;
    doneEvent.reset();
    t0=Time::now();

    return true;
}


/**********************************************************************/
void OnlineStictionEstimator::run()
{
    mutex.wait();

    ienc->getEncoder(joint,&x_pos);

    AWPolyElement el;
    el.data.resize(1,x_pos);
    el.time=Time::now();
    Vector vel=velEst.estimate(el); x_vel=vel[0];
    Vector acc=accEst.estimate(el); x_acc=acc[0];

    double t=Time::now()-t0;
    if (t>2.0*trajGen.getT())
    {
        tg=(tg==x_min)?x_max:x_min;
        state=(tg-x_pos>0.0)?rising:falling;
        adapt=(fabs(x_vel)<vel_thres);
        t0=Time::now();
    }

    trajGen.computeNextValues(Vector(1,tg));
    xd_pos=trajGen.getPos()[0];        

    Vector pid_out=pid->compute(Vector(1,xd_pos),Vector(1,x_pos));
    double e_pos=xd_pos-x_pos;
    double fw=(state==rising)?stiction[0]:stiction[1];
    double u=fw+pid_out[0];

    Vector adaptGate(stiction.length(),0.0);
    if ((fabs(x_vel)<vel_thres) && adapt)
        adaptGate[(state==rising)?0:1]=1.0;
    else
        adapt=false;

    Vector cumErr=intErr.integrate(e_pos*adaptGate);

    // trigger on falling edge
    if (!adapt && adaptOld)
    {
        Vector e_mean=cumErr/t;
        if (yarp::math::norm(e_mean)>e_thres)
        {
            stiction+=gamma*e_mean;
            done[state]=0.0;
        }
        else
            done[state]=1.0;

        intErr.reset(Vector(stiction.length(),0.0));
    }

    ipid->setOffset(joint,dpos_dV*u);
    adaptOld=adapt;

    // fill in info
    info.unput("voltage");   info.put("voltage",u);
    info.unput("position");  info.put("position",x_pos);
    info.unput("reference"); info.put("reference",xd_pos);

    mutex.post();

    if (done[0]*done[1]!=0.0)
        doneEvent.signal();
}


/**********************************************************************/
void OnlineStictionEstimator::threadRelease()
{
    ipid->setOffset(joint,0.0);
    imod->setPositionMode(joint);
    delete pid;

    doneEvent.signal();
}


/**********************************************************************/
bool OnlineStictionEstimator::isDone()
{
    if (!configured)
        return false;

    mutex.wait();
    bool ret=(done[0]*done[1]!=0.0);
    mutex.post();

    return ret;
}


/**********************************************************************/
bool OnlineStictionEstimator::waitUntilDone()
{
    if (!configured)
        return false;

    doneEvent.wait();
    return isDone();
}


/**********************************************************************/
bool OnlineStictionEstimator::getResults(Vector &results)
{
    if (!configured)
        return false;

    mutex.wait();
    results=stiction;
    mutex.post();

    return true;
}


/**********************************************************************/
bool OnlineStictionEstimator::getInfo(Property &info)
{
    if (!configured)
        return false;

    mutex.wait();
    info=this->info;
    mutex.post();

    return true;
}


/**********************************************************************/
OnlineCompensatorDesign::OnlineCompensatorDesign() : RateThread(1000),
                         predictor(Matrix(2,2),Matrix(2,1),Matrix(1,2),
                                   Matrix(2,2),Matrix(1,1))
{
    imod=NULL;
    ilim=NULL;
    ienc=NULL;
    ipos=NULL;
    ipid=NULL;    
    configured=false;
}


/**********************************************************************/
bool OnlineCompensatorDesign::configure(PolyDriver &driver, const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (!driver.isValid())
        return false;

    bool ok=true;
    ok&=driver.view(imod);
    ok&=driver.view(ilim);
    ok&=driver.view(ienc);
    ok&=driver.view(ipos);
    ok&=driver.view(ipid);

    if (!ok)
        return false;

    // general options
    Bottle &optGeneral=opt.findGroup("general");
    if (optGeneral.isNull())
        return false;

    if (!optGeneral.check("joint"))
        return false;

    joint=optGeneral.find("joint").asInt();
    
    if (optGeneral.check("port"))
    {
        string name=optGeneral.find("port").asString().c_str();
        if (name[0]!='/')
            name="/"+name;

        if (!port.open(name.c_str()))
            return false;
    }

    // configure plant estimator
    Bottle &optPlant=opt.findGroup("plant_estimation");
    if (optPlant.isNull())
        return false;

    Pid pidInfo;
    ipid->getPid(joint,&pidInfo);
    dpos_dV=(pidInfo.kp>=0.0?-1.0:1.0);
    
    ilim->getLimits(joint,&x_min,&x_max);
    double x_range=x_max-x_min;
    x_min+=0.1*x_range;
    x_max-=0.1*x_range;

    x0.resize(4,0.0);
    x0[2]=optPlant.check("tau",Value(1.0)).asDouble();
    x0[3]=optPlant.check("K",Value(1.0)).asDouble();

    double Ts=optPlant.check("Ts",Value(0.01)).asDouble();
    double Q=optPlant.check("Q",Value(1.0)).asDouble();
    double R=optPlant.check("R",Value(1.0)).asDouble();
    P0=optPlant.check("P0",Value(1e5)).asDouble();
    max_pwm=optPlant.check("max_pwm",Value(800)).asDouble();

    setRate((int)(1000.0*Ts));

    if (!plant.init(Ts,Q,R,P0,x0))
        return false;

    // configure stiction estimator
    Bottle &optStiction=opt.findGroup("plant_stiction");
    if (!optStiction.isNull())
    {
        Property propStiction(optStiction.toString().c_str());

        // enforce the equality between the common properties
        propStiction.put("joint",joint);

        if (!stiction.configure(driver,propStiction))
            return false;
    }

    meanParams.resize(2,0.0);

    return configured=true;
}


/**********************************************************************/
bool OnlineCompensatorDesign::threadInit()
{
    switch (mode)
    {
        // -----
        case plant_estimation:
        {
            imod->setOpenLoopMode(joint);
            ienc->getEncoder(joint,&x0[0]);
            plant.init(P0,x0);
            meanParams=0.0;
            meanCnt=0;
            x_tg=x_max;
            pwm_pos=true;
            break;
        }

        // -----
        case plant_validation:
        {
            Vector _x0(2,0.0);
            imod->setOpenLoopMode(joint);
            ienc->getEncoder(joint,&_x0[0]);
            predictor.init(_x0,predictor.get_P());
            measure_update_cnt=0;
            x_tg=x_max;
            pwm_pos=true;
            break;
        }

        // -----
        case stiction_estimation:
        {
            stiction.startEstimation();            
            break;
        }

        // -----
        case controller_validation:
        {
            imod->setPositionMode(joint);
            x_tg=x_max;
            if (controller_validation_ref_square)
                ipid->setReference(joint,x_tg);
            else
            {
                ipos->setRefAcceleration(joint,1e9);
                ipos->setRefSpeed(joint,(x_max-x_min)/controller_validation_ref_period);
                ipos->positionMove(joint,x_tg);
            }
            t1=Time::now();
            break;
        }
    }

    doneEvent.reset();
    t0=Time::now();

    return true;
}


/**********************************************************************/
void OnlineCompensatorDesign::commandJoint(double &enc, double &u)
{
    ienc->getEncoder(joint,&enc);

    // switch logic
    if (x_tg==x_max)
    {
        if (enc>x_max)
        {
            x_tg=x_min;
            pwm_pos=false;
        }
    }
    else if (enc<x_min)
    {
        x_tg=x_max;
        pwm_pos=true;
    }

    u=(pwm_pos?max_pwm:-max_pwm);
    ipid->setOffset(joint,dpos_dV*u);
}


/**********************************************************************/
void OnlineCompensatorDesign::run()
{
    double t=Time::now();
    if (max_time>0.0)
        if (t-t0>max_time)
            askToStop();

    mutex.wait();
    switch (mode)
    {
        // -----
        case plant_estimation:
        {
            double enc,u;
            commandJoint(enc,u);
            plant.estimate(u,enc);

            // average parameters tau and K
            meanParams*=meanCnt;
            meanParams+=plant.get_parameters();
            meanParams/=meanCnt++;

            if (port.getOutputCount()>0)
            {
                Vector info(2);
                info[0]=u;
                info[1]=enc;
                info=cat(info,plant.get_x());
                info=cat(info,meanParams);
                port.write(info);
            }

            break;
        }

        // -----
        case plant_validation:
        {
            double enc,u;
            commandJoint(enc,u);
            predictor.predict(Vector(1,u));

            // correction only when requested
            if (measure_update_ticks>0)
            {
                if (++measure_update_cnt>=measure_update_ticks)
                {
                    predictor.correct(Vector(1,enc));
                    measure_update_cnt=0;
                }
            }

            if (port.getOutputCount()>0)
            {
                Vector info(2);
                info[0]=u;
                info[1]=enc;
                info=cat(info,predictor.get_x());
                info=cat(info,Vector(4,0.0));   // zero-padding
                port.write(info);
            }

            break;
        }

        // -----
        case stiction_estimation:
        {
            if (stiction.isDone())
                askToStop();

            if (port.getOutputCount()>0)
            {
                Property stiction_info;
                stiction.getInfo(stiction_info);

                Vector info(3);
                info[0]=stiction_info.find("voltage").asDouble();
                info[1]=stiction_info.find("position").asDouble();
                info[2]=stiction_info.find("reference").asDouble();

                Vector results;
                stiction.getResults(results);
                info=cat(info,results);
                info=cat(info,Vector(3,0.0));   // zero-padding

                port.write(info);
            }

            break;
        }

        // -----
        case controller_validation:
        {
            if (t-t1>controller_validation_ref_period)
            {
                x_tg=(x_tg==x_max?x_min:x_max);
                t1=t;

                if (controller_validation_ref_square)
                    ipid->setReference(joint,x_tg);
                else
                    ipos->positionMove(joint,x_tg);                
            }

            if (port.getOutputCount()>0)
            {
                Vector info(3);
                ipid->getOutput(joint,&info[0]);
                ienc->getEncoder(joint,&info[1]);
                ipid->getReference(joint,&info[2]);
                info=cat(info,Vector(5,0.0));   // zero-padding

                port.write(info);
            }

            break;
        }
    }
    mutex.post();
}


/**********************************************************************/
void OnlineCompensatorDesign::threadRelease()
{
    switch (mode)
    {
        // -----
        case plant_estimation:
        // -----
        case plant_validation:
        {
            ipid->setOffset(joint,0.0);
            imod->setPositionMode(joint);
            break;
        }

        // -----
        case stiction_estimation:
        {
            stiction.stopEstimation();
            break;
        }

        // -----
        case controller_validation:
        {
            ipos->stop(joint);
            break;
        }
    }

    doneEvent.signal();
}


/**********************************************************************/
bool OnlineCompensatorDesign::tuneController(const Property &options,
                                             Property &results)
{
    Property &opt=const_cast<Property&>(options);
    if (!opt.check("tau") || !opt.check("K") || !opt.check("type"))
        return false;

    double tau=opt.find("tau").asDouble();
    double K=opt.find("K").asDouble();
    string type=opt.check("type",Value("P")).asString().c_str();
    double omega,zeta;
    double Kp,Kd,tau_d;

    // P design
    if (strcmpi(type.c_str(),"P")==0)
    {
        if (opt.check("f_cut"))
        {
            omega=2.0*M_PI*opt.find("f_cut").asDouble();
            zeta=1.0/(2.0*tau*omega);
        }
        else if (opt.check("zeta"))
        {
            zeta=opt.find("zeta").asDouble();
            omega=1.0/(2.0*tau*zeta);
        }
        else
            return false;

        Kp=(omega*omega*tau)/K;
        Kd=tau_d=0.0;
    }
    // PD design
    else if (strcmpi(type.c_str(),"PD")==0)
    {
        omega=2.0*M_PI*opt.check("f_cut",Value(2.0*M_PI*2.0)).asDouble();
        zeta=opt.check("zeta",Value(1.0)).asDouble();
        zeta=std::max(zeta,1.0/(2.0*tau*omega));

        Kp=omega/(2.0*zeta*K);
        tau_d=1.0/(2.0*zeta*omega);
        Kd=(tau/tau_d-1.0)/(4.0*zeta*zeta*K);
    }
    else
        return false;

    results.clear();
    results.put("Kp",Kp);
    results.put("Kd",Kd);
    results.put("tau_d",tau_d);
    results.put("f_cut",omega/(2.0*M_PI));
    results.put("zeta",zeta);

    return true;
}


/**********************************************************************/
bool OnlineCompensatorDesign::startPlantEstimation(const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (!configured)
        return false;
    
    if (opt.check("max_time"))
        max_time=opt.find("max_time").asDouble();
    else
        max_time=0.0;

    mode=plant_estimation;
    return RateThread::start();
}


/**********************************************************************/
bool OnlineCompensatorDesign::startPlantValidation(const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (!configured || !opt.check("tau") || !opt.check("K"))
        return false;
    
    if (opt.check("max_time"))
        max_time=opt.find("max_time").asDouble();
    else
        max_time=0.0;

    if (opt.check("measure_update_ticks"))
        measure_update_ticks=opt.find("measure_update_ticks").asInt();
    else
        measure_update_ticks=100;

    double tau=opt.find("tau").asDouble();
    double K=opt.find("K").asDouble();
    double Ts=0.001*getRate();
    double a=1.0/tau;
    double b=K/tau;

    double Q=opt.check("Q",Value(1.0)).asDouble();
    double R=opt.check("R",Value(1.0)).asDouble();
    double P0=opt.check("P0",Value(this->P0)).asDouble();

    // set up the Kalman filter
    double _exp=exp(-Ts*a);
    double _exp_1=1.0-_exp;

    Matrix A=eye(2,2);
    A(0,1)=_exp_1/a;
    A(1,1)=_exp;

    Matrix B(2,1);
    B(0,0)=b*(a*Ts-_exp_1)/(a*a);
    B(1,0)=b*A(0,1);

    Matrix H(1,2);
    H(0,0)=1.0;
    H(0,1)=0.0;

    predictor.set_A(A);
    predictor.set_B(B);
    predictor.set_H(H);
    predictor.set_Q(Q*eye(2,2));
    predictor.set_R(R*eye(1,1));
    predictor.init(Vector(2,0.0),P0*eye(2,2));

    mode=plant_validation;
    return RateThread::start();
}


/**********************************************************************/
bool OnlineCompensatorDesign::startStictionEstimation(const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (!configured)
        return false;

    if (opt.check("max_time"))
        max_time=opt.find("max_time").asDouble();
    else
        max_time=0.0;

    mode=stiction_estimation;
    return RateThread::start();
}


/**********************************************************************/
bool OnlineCompensatorDesign::startControllerValidation(const Property &options)
{
    Property &opt=const_cast<Property&>(options);
    if (!configured)
        return false;

    if (opt.check("max_time"))
        max_time=opt.find("max_time").asDouble();
    else
        max_time=0.0;
    
    ipid->getPid(joint,&pidOld);
    Pid pidNew=pidOld;

    if (opt.check("Kp"))
    {
        // enforce the correct sign of Kp
        double Kp=opt.find("Kp").asDouble();
        Kp=(Kp*pidOld.kp>0.0)?Kp:-Kp;
        pidNew.setKd(Kp);
    }

    if (opt.check("stiction"))
    {
        if (Bottle *pB=opt.find("stiction").asList())
        {
            if (pB->size()>=2)
            {
                double pos=pB->get(0).asDouble();
                double neg=pB->get(1).asDouble();
                pidNew.setStictionValues(pos,neg);
            }
        }
    }

    controller_validation_ref_square=(opt.check("ref_type",Value("square")).asString()=="square");
    controller_validation_ref_period=opt.check("ref_period",Value(2.0)).asDouble();

    mode=controller_validation;
    return RateThread::start();
}


/**********************************************************************/
bool OnlineCompensatorDesign::isDone()
{
    if (!configured)
        return false;

    return !isRunning();
}


/**********************************************************************/
bool OnlineCompensatorDesign::waitUntilDone()
{
    if (!configured)
        return false;

    doneEvent.wait();
    return isDone();
}


/**********************************************************************/
bool OnlineCompensatorDesign::getResults(Property &results)
{
    if (!configured)
        return false;

    results.clear();

    mutex.wait();
    switch (mode)
    {
        // -----
        case plant_estimation:
        {
            Vector params=plant.get_parameters();
            results.put("tau",params[0]);
            results.put("K",params[1]);
            results.put("tau_mean",meanParams[0]);
            results.put("K_mean",meanParams[1]);
            break;
        }

        // -----
        case plant_validation:
        {
            Vector response=predictor.get_x();
            results.put("position",response[0]);
            results.put("velocity",response[1]);
            break;
        }

        // -----
        case stiction_estimation:
        {
            Vector values;
            stiction.getResults(values);
            ostringstream str;
            str<<"( ";
            str<<values[0];
            str<<" ";
            str<<values[1];
            str<<" )";
            results.put("stiction",Value(str.str().c_str()));
            break;
        }

        // -----
        case controller_validation:
        {
            Vector info(3);
            ipid->getOutput(joint,&info[0]);
            ienc->getEncoder(joint,&info[1]);
            ipid->getReference(joint,&info[2]);

            results.put("voltage",info[0]);
            results.put("position",info[1]);
            results.put("reference",info[2]);
            break;
        }
    }
    mutex.post();

    return true;
}


/**********************************************************************/
OnlineCompensatorDesign::~OnlineCompensatorDesign()
{
    port.close();
}




