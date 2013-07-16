﻿#include <LCUI_Build.h>
#include LC_LCUI_H
#include LC_WIDGET_H

#include "game_object.h"
#include "physics_system.h"

#include <time.h>

#define PAUSE	0
#define PLAY	1

enum FuncUse {
	AT_ACTION_DONE = 0,
	AT_FRAME_UPDATE,
	AT_XSPEED_TO_ZERO,
	AT_LANDING
};

/** 动作记录 */
typedef struct ActionRec_ {
	int id;			/**< 动画的标识号 */
	ActionData *action;	/**< 对应的动作集 */
} ActionRec;

/** 每次数据刷新时的时间间隔(毫秒) */
#define REFRESH_INTERVAL_TIME 20

typedef struct GameObject_ {
	int state;			/**< 当前状态 */
	int x, y;			/**< 底线中点坐标 */
	int w, h;			/**< 动作容器的尺寸 */
	int global_bottom_line_y;	/**< 底线的Y轴坐标 */
	int global_center_x;		/**< 中心点的X轴坐标 */
	LCUI_BOOL data_valid;		/**< 当前使用的数据是否有效 */
	LCUI_BOOL horiz_flip;		/**< 是否水平翻转 */
	ActionRec *current;		/**< 当前动作动画记录 */
	LCUI_Queue action_list;		/**< 动作列表 */
	int n_frame;			/**< 记录当前帧动作的序号，帧序号从0开始 */
	long int remain_time;		/**< 当前帧剩下的停留时间 */
	LCUI_Func func[4];		/**< 被关联的回调函数 */
	PhysicsObject *phys_obj;	/**< 对应的物理对象 */
	LCUI_Widget *shadow;		/**< 对象的阴影 */
} GameObject;

static LCUI_Queue action_database;
static LCUI_Queue gameobject_stream;

static int database_init = FALSE;
static int frame_proc_timer = -1;

static void GameObject_CallFunc( LCUI_Widget *widget, int func_use )
{
	GameObject *obj;
	obj = (GameObject*)Widget_GetPrivData( widget );
	AppTasks_CustomAdd( ADD_MODE_NOT_REPEAT | AND_ARG_F, &obj->func[func_use] );
}

static void ActionData_Destroy( void *arg )
{
	ActionData *action;
	action = (ActionData*)arg;
	Queue_Destroy( &action->frame );
}


/**
 * 创建一个动作集
 * 创建的动作集将记录至动作库中
 * @returns
 *	正常则返回指向动作库中的该动作集的指针，失败则返回NULL
 */
LCUI_API ActionData* Action_Create( void )
{
	int pos;
	ActionData *p, action;

	Queue_Init( &action.frame, sizeof(ActionFrameData), NULL );
	
	if( !database_init ) {
		Queue_Init(	&action_database,
				sizeof(ActionData),
				ActionData_Destroy );
		database_init = TRUE;
	}
	Queue_Lock( &action_database );
	/* 记录该动画至库中 */
	pos = Queue_Add( &action_database, &action );
	p = (ActionData*)Queue_Get( &action_database, pos );
	Queue_Unlock( &action_database );
	DEBUG_MSG("create action: %p\n", p);
	return p;
}

/**
 * 删除一个动画
 * 从动画库中删除指定的动画
 * @param action
 *	需删除的动画
 * @returns
 *	正常则返回0，失败则返回-1
 */
LCUI_API int Action_Delete( ActionData* action )
{
	int n;
	ActionData* tmp;

	Queue_Lock( &action_database );
	n = Queue_GetTotal( &action_database );
	for(n; n>=0; --n) {
		tmp = (ActionData*)Queue_Get( &action_database, n );
		if( tmp == action ) {
			Queue_Delete( &action_database, n );
			break;
		}
	}
	Queue_Unlock( &action_database );
	if( n < 0 ) {
		return -1;
	}
	return 0;
}

static int GameObject_Connect(	LCUI_Widget *widget,
				int func_use,
				void (*func)(LCUI_Widget*) )
{
	GameObject *obj;
	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->func[func_use].func = (CallBackFunc)func;
	obj->func[func_use].id = LCUIApp_GetSelfID();
	obj->func[func_use].arg[0] = widget;
	obj->func[func_use].arg[1] = NULL;
	obj->func[func_use].destroy_arg[0] = FALSE;
	obj->func[func_use].destroy_arg[1] = FALSE;
	return 0;
}


LCUI_API void GameObject_AtActionDone(	LCUI_Widget *widget,
					void (*func)(LCUI_Widget*) )
{
	GameObject_Connect( widget, AT_ACTION_DONE, func );
}

LCUI_API void GameObject_AtXSpeedToZero(	LCUI_Widget *widget,
						double acc,
						void (*func)(LCUI_Widget*) )
{
	GameObject_SetXAcc( widget, acc );
	GameObject_Connect( widget, AT_XSPEED_TO_ZERO, func );
}

/** 设置在对象着地时进行响应 */
LCUI_API void GameObject_AtLanding(	LCUI_Widget *widget,
					double z_speed,
					double z_acc,
					void (*func)(LCUI_Widget*) )
{
	GameObject_SetZSpeed( widget, z_speed );
	GameObject_SetZAcc( widget, z_acc );
	GameObject_Connect( widget, AT_LANDING, func );
}

/** 对流中的GameObject按照剩余时间从小到大的顺序排序  */
static void GameObjectStream_Sort(void)
{
	int i, j, total;
	LCUI_Widget *widget;
	GameObject *p1, *p2;

	Queue_Lock( &gameobject_stream );
	total = Queue_GetTotal( &gameobject_stream );
	for(i=0; i<total; ++i) {
		widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, i );
		p1 = (GameObject*)Widget_GetPrivData( widget );
		if( !p1 ) {
			continue;
		}
		for(j=i+1; j<total; ++j) {
			widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, j );
			p2 = (GameObject*)Widget_GetPrivData( widget );
			if( !p2 ) {
				continue;
			}
			if( p1->remain_time > p2->remain_time ) {
				p1 = p2;
				Queue_Swap( &gameobject_stream, j, i );
			}
		}
	}
	Queue_Unlock( &gameobject_stream );
}

static void GameObjectStream_TimeSub( int time )
{
	int i, total;
	LCUI_Widget *widget;
	GameObject *obj;

	Queue_Lock( &gameobject_stream );
	total = Queue_GetTotal(&gameobject_stream);
	DEBUG_MSG("start\n");
	for(i=0; i<total; ++i) {
		widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, i );
		obj = (GameObject*)Widget_GetPrivData( widget );
		if( !obj || obj->state == PAUSE ) {
			continue;
		}
		obj->remain_time -= time;
	}
	Queue_Unlock( &gameobject_stream );
}

static long int Action_GetFrameSleepTime( ActionData *action, int n_frame )
{
	ActionFrameData *frame;
	frame = (ActionFrameData*)Queue_Get( &action->frame, n_frame );
	if( frame ) {
		return frame->sleep_time;
	}
	return REFRESH_INTERVAL_TIME;
}

/** 更新流中的动画当前帧的停留时间 */
static void GameObjectStream_UpdateTime( int sleep_time )
{
	int i, n, total;
	LCUI_BOOL need_draw=FALSE;
	LCUI_Widget *widget;
	GameObject *obj;
	ActionFrameData *frame = NULL;
	/* 减少所有GameObject当前帧的剩余等待时间 */
	GameObjectStream_TimeSub( sleep_time );
	total = Queue_GetTotal(&gameobject_stream);
	for(i=0; i<total; ++i){
		widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, i );
		obj = (GameObject*)Widget_GetPrivData( widget );
		/* 忽略无效或者未处于播放状态的对象 */
		if( !obj || obj->state != PLAY) {
			continue;;
		}
		/* 忽略没有动作动画的对象 */
		if( obj->current == NULL ) {
			continue;
		}
		/* 若当前帧的停留时间小于或等于0 */
		if(obj->remain_time <= 0) {
			++obj->n_frame;
			/* 记录新一帧动作的总停留时间 */
			obj->remain_time = Action_GetFrameSleepTime(
						obj->current->action,
						obj->n_frame
			);
			GameObject_CallFunc( widget, AT_FRAME_UPDATE );
			/* 标记这个对象需要重绘 */
			need_draw = TRUE;
		}
		n = Queue_GetTotal( &obj->current->action->frame );
		/* 若当前帧号超出总帧数 */
		if( obj->n_frame >= n ) {
			obj->n_frame = 0;
			obj->remain_time = Action_GetFrameSleepTime(
						obj->current->action,
						obj->n_frame
			);
			/* 当前动作动画已完成一遍播放，调用回调函数来
			 * 响应AT_ACTION_DONE信号 */
			GameObject_CallFunc( widget, AT_ACTION_DONE );
		}
		if( need_draw ) {
			Widget_Draw( widget );
		}
	}
	GameObjectStream_Sort();
}

/** 获取对象的受攻击范围 */
static int GameObject_GetHitRange( GameObject *obj, RangeBox *range )
{
	ActionFrameData *frame;
	if( obj->current == NULL ) {
		return -1;
	}
	frame = (ActionFrameData*)Queue_Get(
			&obj->current->action->frame,
			obj->n_frame
	);
	if( frame == NULL ) {
		return -2;
	}
	if( frame->hitbox.x_width <= 0
	 || frame->hitbox.y_width <= 0
	 || frame->hitbox.z_width <= 0) {
		 return -3;
	}
	/* 动作图是否水平翻转，只影响范围框的X轴坐标 */
	if( obj->horiz_flip ) {
		range->x = (int)obj->phys_obj->x - frame->hitbox.x;
		range->x -= frame->hitbox.x_width;
	} else {
		range->x = (int)obj->phys_obj->x + frame->hitbox.x;
	}
	range->x_width = frame->hitbox.x_width;
	range->y = (int)obj->phys_obj->y + frame->hitbox.y;
	range->y_width = frame->hitbox.y_width;
	range->z = (int)obj->phys_obj->z + frame->hitbox.z;
	range->z_width = frame->hitbox.z_width;
	return 0;
}

/** 获取对象的攻击范围 */
static int GameObject_GetAttackRange( GameObject *obj, RangeBox *range )
{
	ActionFrameData *frame;
	if( obj->current == NULL ) {
		return -1;
	}
	frame = (ActionFrameData*)Queue_Get(
			&obj->current->action->frame,
			obj->n_frame
	);
	if( frame == NULL ) {
		return -2;
	}
	if( frame->atkbox.x_width <= 0
	 || frame->atkbox.y_width <= 0
	 || frame->atkbox.z_width <= 0) {
		 return -3;
	}
	/* 动作图是否水平翻转，只影响范围框的X轴坐标 */
	if( obj->horiz_flip ) {
		range->x = (int)obj->phys_obj->x - frame->atkbox.x;
		range->x -= frame->atkbox.x_width;
	} else {
		range->x = (int)obj->phys_obj->x + frame->atkbox.x;
	}
	range->x_width = frame->atkbox.x_width;
	range->y = (int)obj->phys_obj->y + frame->atkbox.y;
	range->y_width = frame->atkbox.y_width;
	range->z = (int)obj->phys_obj->z + frame->atkbox.z;
	range->z_width = frame->atkbox.z_width;
	return 0;
}

/** 判断两个范围是否相交 */
static LCUI_BOOL RangeBox_IsIntersect( RangeBox *range1, RangeBox *range2 )
{
	/* 先判断在X轴上是否有相交 */
	if( range1->x + range1->x_width <= range2->x ) {
		return FALSE;
	}
	if( range2->x + range2->x_width <= range1->x ) {
		return FALSE;
	}
	/* 然后判断在X轴上是否有相交 */
	if( range1->y + range1->y_width <= range2->y ) {
		return FALSE;
	}
	if( range2->y + range2->y_width <= range1->y ) {
		return FALSE;
	}
	/* 最后判断在Z轴上是否有相交 */
	if( range1->z + range1->z_width <= range2->z ) {
		return FALSE;
	}
	if( range2->z + range2->z_width <= range1->z ) {
		return FALSE;
	}
	return TRUE;
}

/** 获取攻击该对象的攻击者 */
static GameObject *GameObject_GetAttacker( GameObject *obj )
{
	int i, n;
	GameObject *attacker_obj;
	LCUI_Widget *widget;
	RangeBox hit_range, attack_range;
	/* 获取该对象的受攻击范围，若获取失败，则返回NULL */
	if( 0 > GameObject_GetHitRange( obj, &hit_range ) ) {
		return NULL;
	}
	n = Queue_GetTotal( &gameobject_stream );
	for(i=0; i<n; ++i) {
		widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, i );
		attacker_obj = (GameObject*)Widget_GetPrivData( widget );
		if( !attacker_obj || attacker_obj == obj ) {
			continue;
		}
		/* 获取对象的攻击范围，若获取失败，则继续判断下个对象 */
		if( 0 > GameObject_GetAttackRange( attacker_obj, &attack_range ) ) {
			continue;
		}
		/* 若两个范围相交 */
		if( RangeBox_IsIntersect(&hit_range, &attack_range) ) {
			return attacker_obj;
		}
	}
	return NULL;
}

static void GameObjectStream_Proc( void* arg )
{
	GameObject *obj, *attacker;
	LCUI_Widget *widget;
	int i, n, lost_time;
	static clock_t current_time = 0;

	while(!LCUI_Active()) {
		LCUI_MSleep(10);
	}
	lost_time = clock() - current_time;
	//_DEBUG_MSG("%d\n", lost_time);
	current_time = clock();
	GameObjectStream_UpdateTime( lost_time );
	PhysicsSystem_Step();
	n = Queue_GetTotal( &gameobject_stream );
	for(i=0; i<n; ++i) {
		widget = (LCUI_Widget*)Queue_Get( &gameobject_stream, i );
		obj = (GameObject*)Widget_GetPrivData( widget );
		if( !obj || obj->state != PLAY ) {
			continue;
		}
		/* 获取命中当前对象的攻击者 */
		attacker = GameObject_GetAttacker( obj );
		if( attacker ) {
			_DEBUG_MSG("victim: %p, attacker: %p\n", obj, attacker);
		}
		/* 若速度接近0 */
		if( (obj->phys_obj->x_acc > 0
		 && obj->phys_obj->x_speed >= 0 )
		|| (obj->phys_obj->x_acc < 0
		 && obj->phys_obj->x_speed <= 0 ) ) {
			obj->phys_obj->x_acc = 0;
			obj->phys_obj->x_speed = 0;
			Widget_Update( widget );
			GameObject_CallFunc( widget, AT_XSPEED_TO_ZERO );
		}
		/**
		目前假设地面的Z坐标为0，当对象的Z坐标达到0时就认定它着陆了。
		暂不考虑其他对象对当前对象的着陆点的影响。
		**/
		if( (obj->phys_obj->z_acc > 0
		 && obj->phys_obj->z > 0 )
		|| (obj->phys_obj->z_acc < 0
		 && obj->phys_obj->z < 0 ) ) {
			 obj->phys_obj->z = 0;
			obj->phys_obj->z_acc = 0;
			obj->phys_obj->z_speed = 0;
			Widget_Update( widget );
			GameObject_CallFunc( widget, AT_LANDING );
		}
	}
}

/**
 * 切换对象的动作
 * @param widget
 *	目标ActiveBox部件
 * @param action_id
 *	切换至的动画的标识号
 * @return
 *	切换成功则返回0，未找到指定ID的动画记录，则返回-1
 */
LCUI_API int GameObject_SwitchAction(	LCUI_Widget *widget,
					int action_id )
{
	int i, n;
	ActionRec *p_rec;
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	n = Queue_GetTotal( &obj->action_list );
	for(i=0; i<n; ++i) {
		p_rec = (ActionRec*)Queue_Get( &obj->action_list, i );
		if( !p_rec || p_rec->id != action_id ) {
			continue;
		}
		obj->current = p_rec;
		obj->n_frame = 0;
		obj->remain_time = Action_GetFrameSleepTime( p_rec->action, 0 );
		/* 标记数据为无效，以根据当前动作动画来更新数据 */
		obj->data_valid = FALSE;
		Widget_Draw(widget);
		return 0;
	}
	return -1;
}

/** 获取当前动作动画的ID */
LCUI_API int GameObject_GetCurrentActionID( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	if( obj->current == NULL ) {
		obj->current = (ActionRec*)Queue_Get( &obj->action_list, 0 );
		if( obj->current == NULL ) {
			return -1;
		}
	}
	return obj->current->id;
}

LCUI_API int GameObject_ResetAction( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData(widget);
	if( obj->current == NULL ) {
		obj->current = (ActionRec*)Queue_Get( &obj->action_list, 0 );
		if( obj->current == NULL ) {
			return -1;
		}
	}
	obj->n_frame = 0;
	obj->remain_time = Action_GetFrameSleepTime( obj->current->action, 0 );
	Widget_Draw( widget );
	return 0;
}

/** 播放对象的动作 */
LCUI_API int GameObject_PlayAction( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData(widget);
	if( obj->current == NULL ) {
		obj->current = (ActionRec*)Queue_Get( &obj->action_list, 0 );
		if( obj->current == NULL ) {
			return -1;
		}
	}
	obj->state = PLAY;
	obj->n_frame = 0;
	obj->remain_time = Action_GetFrameSleepTime( obj->current->action, 0 );
	Widget_Draw( widget );
	return 0;
}

/** 暂停对象的动作 */
LCUI_API int GameObject_PauseAction( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData(widget);
	if( obj->current == NULL ) {
		return -1;
	}
	obj->state = PAUSE;
	return 0;
}


/**
 * 为动作添加一帧动作图
 * @param action
 *	目标动作
 * @param offset_x
 *	人物底线中点相对于动作图低边中点的X轴的偏移量
 * @param offset_y
 *	人物底线中点相对于动作图低边中点的Y轴的偏移量
 * @param graph
 *	动作图
 * @param sleep_time
 *	动作图的停留时间，单位时间为20毫秒，即当它的值为50时，该帧动作图停留1秒
 * @return
 *	正常则返回该帧动作图的序号，失败返回-1
 */
LCUI_API int Action_AddFrame(	ActionData* action,
				int offset_x,
				int offset_y,
				LCUI_Graph *graph,
				int sleep_time )
{
	ActionFrameData frame;
	
	frame.sleep_time = sleep_time*REFRESH_INTERVAL_TIME;
	frame.offset.x = offset_x;
	frame.offset.y = offset_y;
	frame.graph = *graph;
	frame.atkbox.x = frame.hitbox.x = 0;
	frame.atkbox.y = frame.hitbox.y = 0;
	frame.atkbox.z = frame.hitbox.z = 0;
	frame.atkbox.x_width = frame.hitbox.x_width = 0;
	frame.atkbox.y_width = frame.hitbox.y_width = 0;
	frame.atkbox.z_width = frame.hitbox.z_width = 0;

	return Queue_Add( &action->frame, &frame );
}

LCUI_API int Action_SetAttackRange(	ActionData* action,
					int n_frame,
					RangeBox attack_range )
{
	ActionFrameData* p_frame;
	if( n_frame < 0 ) {
		return -1;
	}
	p_frame = (ActionFrameData*)Queue_Get( &action->frame, n_frame );
	p_frame->atkbox = attack_range;
	return 0;
}

LCUI_API int Action_SetHitRange(	ActionData* action,
					int n_frame,
					RangeBox hit_range )
{
	ActionFrameData* p_frame;
	if( n_frame < 0 ) {
		return -1;
	}
	p_frame = (ActionFrameData*)Queue_Get( &action->frame, n_frame );
	p_frame->hitbox = hit_range;
	return 0;
}

static void GameObject_ExecInit( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_NewPrivData( widget, sizeof(GameObject) );
	
	obj->x = 0;
	obj->y = 0;
	obj->w = 0;
	obj->h = 0;
	obj->state = PAUSE;
	obj->n_frame = 0;
	obj->remain_time = 0;
	obj->current = NULL;
	obj->data_valid = FALSE;
	obj->global_center_x = 0;
	obj->global_bottom_line_y = 0;
	obj->horiz_flip = FALSE;
	obj->phys_obj = PhysicsObject_New(0,0,0,0,0,0 );
	obj->shadow = Widget_New(NULL);

	Queue_Init( &obj->action_list, sizeof(ActionRec), NULL );
	if( frame_proc_timer == -1 ) {
		Queue_Init( &gameobject_stream, sizeof(LCUI_Widget*), NULL );
		Queue_UsingPointer( &gameobject_stream );
		frame_proc_timer = LCUITimer_Set( 
			REFRESH_INTERVAL_TIME,
			GameObjectStream_Proc,
			NULL, TRUE
		);
	}
	Queue_AddPointer( &gameobject_stream, widget );
	obj->func[0].func = NULL;
	obj->func[1].func = NULL;
	obj->func[2].func = NULL;
	obj->func[3].func = NULL;
}

static void GameObject_ExecHide( LCUI_Widget *widget )
{
	GameObject *obj;
	obj = (GameObject*)Widget_GetPrivData( widget );
	Widget_Hide( obj->shadow );
}

static void GameObject_ExecShow( LCUI_Widget *widget )
{
	GameObject *obj;
	obj = (GameObject*)Widget_GetPrivData( widget );
	Widget_Show( obj->shadow );
}

static void GameObject_ExecDestroy( LCUI_Widget *widget )
{
	GameObject *obj;
	obj = (GameObject*)Widget_GetPrivData( widget );
	Widget_Destroy( obj->shadow );
}

/** 获取当前动作信息 */
static int GameObject_UpdateData( LCUI_Widget *widget )
{
	int i, n;
	GameObject *obj;
	LCUI_Queue *frame_list;
	ActionFrameData *frame;
	int frame_bottom_y, frame_center_x;
	int box_w=0, box_h=0, point_x=0, point_y=0;

	obj = (GameObject*)Widget_GetPrivData( widget );
	if( obj->current == NULL ) {
		return -1;
	}
	if( obj->current->action == NULL ) {
		return -1;
	}

	frame_list = &obj->current->action->frame;
	n = Queue_GetTotal( &obj->current->action->frame );
	for( i=0; i<n; ++i ) {
		frame = (ActionFrameData*)Queue_Get( frame_list, i );
		if( frame == NULL ) {
			continue;
		}
		/*
		 * frame->graph是当前帧动作的容器
		 * frame->graph.w 和 frame->graph.h 则是该容器的尺寸
		 * box_w 和 box_h 是该动作集的容器尺寸
		 * frame_bottom_y 是对象底线在当前帧的容器中的Y轴坐标
		 * frame_center_x 是对象中点在当前帧的容器中的X轴坐标
		 * point_y 和 point_x 与上面两个类似，但坐标是相对动作集的容器
		 */
		frame_bottom_y = frame->graph.h + frame->offset.y;
		frame_center_x = frame->graph.w/2 + frame->offset.x;
		/* 对比容器顶端到底线的距离 */
		if( frame_bottom_y > point_y ) {
			box_h = (box_h - point_y) + frame_bottom_y;
			point_y = frame_bottom_y;
		}
		/* 对比底线到容器底端的距离 */
		if( frame->graph.h - frame_bottom_y > box_h - point_y  ) {
			box_h = point_y + (frame->graph.h - frame_bottom_y);
		}
		/* 对比容器左端到中心点的距离 */
		if( frame_center_x > point_x ) {
			box_w = (box_w - point_x) + frame_center_x;
			point_x = frame_center_x;
		}
		/* 对比中心点到容器右端的距离 */
		if( frame->graph.w - frame_center_x > box_w - point_x ) {
			box_w = point_x + (frame->graph.w - frame_center_x);
		}
	}
	obj->global_center_x = point_x;
	obj->global_bottom_line_y = point_y;
	obj->w = box_w;
	obj->h = box_h;
	return 0;
}

/** 为对象添加一个动作 */
LCUI_API int GameObject_AddAction(	LCUI_Widget *widget,
					ActionData *action,
					int id )
{
	int ret, i, n;
	ActionRec rec, *p_rec;
	GameObject *obj;

	if( !action ) {
		return -1;
	}
	obj = (GameObject*)Widget_GetPrivData( widget );

	Queue_Lock( &obj->action_list );
	n = Queue_GetTotal( &obj->action_list );
	/* 先寻找是否有相同ID的动作动画 */
	for(i=0; i<n; ++i) {
		p_rec = (ActionRec*)Queue_Get( &obj->action_list, i );
		if( !p_rec ) {
			continue;
		}
		/* 有则覆盖 */
		if( p_rec->id == id ) {
			p_rec->action = action;
			break;
		}
	}
	if( i < n ) {
		Queue_Unlock( &obj->action_list );
		return 0;
	}
	rec.action = action;
	rec.id = id;
	/* 添加至动作列表中 */
	ret = Queue_Add( &obj->action_list, &rec );
	Queue_Unlock( &obj->action_list );
	if( ret < 0 ) {
		return -2;
	}
	return 0;
}

static void GameObject_ExecUpdate( LCUI_Widget *widget )
{
	GameObject *obj;
	LCUI_Pos object_pos, shadow_pos;

	obj = (GameObject*)Widget_GetPrivData( widget );
	if( obj->current == NULL ) {
		obj->current = (ActionRec*)Queue_Get( &obj->action_list, 0 );
		/* 既然没有动作动画，那就不进行更新 */
		if( obj->current == NULL ) {
			return;
		}
	}
	obj->x = (int)obj->phys_obj->x;
	obj->y = (int)(obj->phys_obj->y-obj->phys_obj->z);
	/* 计算部件的坐标 */
	if( obj->horiz_flip ) {
		object_pos.x = obj->x - (obj->w - obj->global_center_x);
	} else {
		object_pos.x = obj->x - obj->global_center_x;
	}
	object_pos.y = obj->y - obj->global_bottom_line_y;
	/* 计算阴影的坐标 */
	shadow_pos.x = (int)(obj->x - obj->shadow->size.w/2);
	shadow_pos.y = (int)(obj->phys_obj->y - obj->shadow->size.h/2)-1;
	/* 调整堆叠顺序 */
	Widget_SetZIndex( widget, (int)obj->phys_obj->y );
	Widget_SetZIndex( obj->shadow, -1000 - (int)obj->phys_obj->y );
	/* 移动部件的位置 */
	Widget_Move( widget, object_pos );
	Widget_Move( obj->shadow, shadow_pos );
	/* 如果数据还有效 */
	if( obj->data_valid ) {
		return;
	}
	/* 如果在数据更新后未变化 */
	if( GameObject_UpdateData( widget ) != 0 ) {
		return;
	}
	if( obj->horiz_flip ) {
		object_pos.x = obj->x - (obj->w - obj->global_center_x);
	} else {
		object_pos.x = obj->x - obj->global_center_x;
	}
	object_pos.y = obj->y - obj->global_bottom_line_y;
	shadow_pos.x = (int)(obj->x - obj->shadow->size.w/2);
	shadow_pos.y = (int)(obj->phys_obj->y - obj->shadow->size.h/2)-1;
	
	Widget_SetZIndex( widget, (int)obj->phys_obj->y );
	Widget_SetZIndex( obj->shadow, -1000 - (int)obj->phys_obj->y );
	Widget_Move( widget, object_pos );
	Widget_Move( obj->shadow, shadow_pos );
	/* 并调整部件的尺寸，以正常显示对象的动画 */
	Widget_Resize( widget, Size(obj->w, obj->h) );
	obj->data_valid = TRUE;
}

static void GameObject_ExecDraw( LCUI_Widget *widget )
{
	GameObject *obj;
	LCUI_Pos pos;
	ActionFrameData *frame;
	LCUI_Graph *graph, img_buff;
	
	obj = (GameObject*)Widget_GetPrivData( widget );
	if( obj->current == NULL ) {
		obj->current = (ActionRec*)Queue_Get( &obj->action_list, 0 );
		if( obj->current == NULL ) {
			return;
		}
	}
	/* 获取当前帧动作图像 */
	frame = (ActionFrameData*)Queue_Get(
			&obj->current->action->frame,
			obj->n_frame
	);
	if( frame == NULL ) {
		return;
	}
	/* 计算当前帧相对于部件的坐标 */
	pos.y = frame->graph.h + frame->offset.y;
	pos.y = obj->global_bottom_line_y - pos.y;
	/* 获取部件自身图层的图像 */
	graph = Widget_GetSelfGraph( widget );
	/* 若需要将当前帧图像进行水平翻转 */
	if( obj->horiz_flip ) {
		Graph_Init( &img_buff );
		pos.x = frame->graph.w/2 - frame->offset.x;
		pos.x = obj->w - obj->global_center_x - pos.x;
		Graph_HorizFlip( &frame->graph, &img_buff );
		/* 绘制到部件上 */
		Graph_Replace( graph, &img_buff, pos );
		Graph_Free( &img_buff );
	} else {
		pos.x = frame->graph.w/2 + frame->offset.x;
		pos.x = obj->global_center_x - pos.x;
		Graph_Replace( graph, &frame->graph, pos );
	}
}

/** 设置对象是否进行水平翻转 */
LCUI_API void GameObject_SetHorizFlip( LCUI_Widget *widget, LCUI_BOOL flag )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	if( obj->horiz_flip != flag ) {
		obj->horiz_flip = flag;
		obj->data_valid = FALSE;
		Widget_Draw( widget );
	}
}

/** 设置相对于X轴的加速度 */
LCUI_API void GameObject_SetXAcc( LCUI_Widget *widget, double acc )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->x_acc = acc;
}

/** 获取相对于X轴的加速度 */
LCUI_API double GameObject_GetXAcc( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->x_acc;
}

/** 设置相对于Y轴的加速度 */
LCUI_API void GameObject_SetYAcc( LCUI_Widget *widget, double acc )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->y_acc = acc;
}

/** 获取相对于Y轴的加速度 */
LCUI_API double GameObject_GetYAcc( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->y_acc;
}

/** 设置相对于Y轴的加速度 */
LCUI_API void GameObject_SetZAcc( LCUI_Widget *widget, double acc )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->z_acc = acc;
}

/** 获取相对于Z轴的加速度 */
LCUI_API double GameObject_GetZAcc( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->z_acc;
}

/** 设置游戏对象在X轴的移动速度 */
LCUI_API void GameObject_SetXSpeed( LCUI_Widget *widget, double x_speed )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->x_speed = x_speed;
	Widget_Update( widget );
}

/** 获取游戏对象在X轴的移动速度 */
LCUI_API double GameObject_GetXSpeed( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->x_speed;
}

/** 设置游戏对象在Y轴的移动速度 */
LCUI_API void GameObject_SetYSpeed( LCUI_Widget *widget, double y_speed )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->y_speed = y_speed;
	Widget_Update( widget );
}

/** 获取游戏对象在Y轴的移动速度 */
LCUI_API double GameObject_GetYSpeed( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->y_speed;
}

/** 设置游戏对象在Z轴的移动速度 */
LCUI_API void GameObject_SetZSpeed( LCUI_Widget *widget, double z_speed )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->z_speed = z_speed;
	Widget_Update( widget );
}

/** 获取游戏对象在Z轴的移动速度 */
LCUI_API double GameObject_GetZSpeed( LCUI_Widget *widget )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	return obj->phys_obj->z_speed;
}

/** 移动游戏对象的位置 */
LCUI_API void GameObject_SetPos( LCUI_Widget *widget, double x, double y )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	obj->phys_obj->x = x;
	obj->phys_obj->y = y;
	Widget_Update( widget );
}

/** 获取游戏对象的位置 */
LCUI_API void GameObject_GetPos( LCUI_Widget *widget, double *x, double *y )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	*x = obj->x;
	*y = obj->y;
}

/** 设置游戏对象的阴影图像 */
LCUI_API void GameObject_SetShadow( LCUI_Widget *widget, LCUI_Graph img_shadow )
{
	GameObject *obj;

	obj = (GameObject*)Widget_GetPrivData( widget );
	Widget_Resize( obj->shadow, Graph_GetSize(&img_shadow) );
	Widget_SetBackgroundImage( obj->shadow, &img_shadow );
}

LCUI_API LCUI_Widget* GameObject_New(void)
{
	return Widget_New("GameObject");
}


LCUI_API void GameObject_Register(void)
{
	WidgetType_Add("GameObject");
	WidgetFunc_Add("GameObject", GameObject_ExecInit, FUNC_TYPE_INIT );
	WidgetFunc_Add("GameObject", GameObject_ExecDraw, FUNC_TYPE_DRAW );
	WidgetFunc_Add("GameObject", GameObject_ExecUpdate, FUNC_TYPE_UPDATE );
	WidgetFunc_Add("GameObject", GameObject_ExecHide, FUNC_TYPE_HIDE );
	WidgetFunc_Add("GameObject", GameObject_ExecShow, FUNC_TYPE_SHOW );
	WidgetFunc_Add("GameObject", GameObject_ExecDestroy, FUNC_TYPE_DESTROY );
}
